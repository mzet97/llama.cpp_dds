#include "dds_bridge.h"

#include "dds_transport.h"

#include <chrono>
#include <condition_variable>
#include <thread>

namespace llama_dds {

class DDSBridgeImpl {
  public:
    DDSBridgeImpl(int domain_id) : domain_id_(domain_id), transport_(std::make_unique<DDSTransport>(domain_id)) {}

    ~DDSBridgeImpl() { stop(); }

    bool start(DDSBridge::ProcessRequestCallback on_request) {
        // Wire the transport directly to the provided callback (DDSBridge::handle_request).
        // Avoids an extra indirection layer between DDSTransport and DDSBridge.
        bool success = transport_->start_server(std::move(on_request));

        if (!success) {
            fprintf(stderr, "[DDSBridge] failed to start DDS transport\n");
            return false;
        }

        running_ = true;

        // Start status publishing thread with cancellable sleep
        worker_thread_ = std::thread([this]() {
            while (running_) {
                {
                    std::unique_lock<std::mutex> lk(stop_mutex_);
                    stop_cv_.wait_for(lk, std::chrono::seconds(5),
                                      [this] { return !running_.load(); });
                }

                if (!running_) {
                    break;
                }

                // Publish slot counts based on current in-flight request count.
                ServerStatus status;
                status.server_id = "llama-dds-server";
                {
                    std::lock_guard<std::mutex> lk(status_mutex_);
                    status.slots_processing = (int32_t) pending_count_.load();
                    // total_slots_ is set by set_model_info() from params.n_parallel.
                    status.slots_idle       = std::max(0, total_slots_ - status.slots_processing);
                    status.model_loaded     = model_loaded_;
                    status.ready            = model_ready_;
                }

                transport_->publish_status(status);
            }
        });

        fprintf(stderr, "[DDSBridge] started successfully\n");
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(stop_mutex_);
            running_ = false;
        }
        stop_cv_.notify_all();

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        transport_->stop_server();
    }

    void update_status(const std::string & server_id,
                       int32_t             slots_idle,
                       int32_t             slots_processing,
                       const std::string & model_loaded,
                       bool                ready) {
        ServerStatus status;
        status.server_id        = server_id;
        status.slots_idle       = slots_idle;
        status.slots_processing = slots_processing;
        status.model_loaded     = model_loaded;
        status.ready            = ready;

        transport_->publish_status(status);
    }

    bool send_response(const ChatCompletionResponse & response) { return transport_->send_response(response); }

    bool send_error(const LLMInferenceError & error) { return transport_->send_error(error); }

    // Update model name, readiness, and parallel-slot capacity for status publishing.
    void set_model_info(const std::string & model_name, bool ready, int n_parallel = 1) {
        std::lock_guard<std::mutex> lk(status_mutex_);
        model_loaded_ = model_name;
        model_ready_  = ready;
        total_slots_  = std::max(1, n_parallel);
    }

    // Increment/decrement the count of in-flight requests, used for status reporting.
    void inc_pending() { pending_count_.fetch_add(1); }

    int pending_count() const { return pending_count_.load(std::memory_order_acquire); }

    void dec_pending() {
        int old = pending_count_.load(std::memory_order_relaxed);
        while (old > 0) {
            if (pending_count_.compare_exchange_weak(old, old - 1,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed)) {
                return;
            }
            // CAS weak-fail-reload pattern is efficient for low contention
        }
    }

    std::unique_ptr<DDSTransport> transport_;

    std::atomic<bool> running_{ false };
    std::atomic<int>  pending_count_{ 0 };  // number of in-flight requests
    int               domain_id_;

    // Model state snapshot for the status-publishing thread.
    mutable std::mutex status_mutex_;
    std::mutex         stop_mutex_;
    std::condition_variable stop_cv_;
    std::string        model_loaded_;
    bool               model_ready_{ false };
    int                total_slots_{ 1 };  // copied from params.n_parallel

    // process_callback_ is unused; kept only for ABI compatibility.
    std::thread worker_thread_;
};

// DDSBridge implementation
DDSBridge::DDSBridge(int domain_id) : domain_id_(domain_id), pimpl_(std::make_unique<DDSBridgeImpl>(domain_id)) {}

DDSBridge::~DDSBridge() = default;

bool DDSBridge::init() {
    initialized_ = true;
    return true;
}

void DDSBridge::set_process_callback(ProcessRequestCallback callback) {
    process_callback_ = std::move(callback);
}

bool DDSBridge::start() {
    if (!initialized_) {
        fprintf(stderr, "[DDSBridge] not initialized\n");
        return false;
    }

    // Pass handle_request as the transport callback, avoiding an extra indirection.
    bool success = pimpl_->start([this](const ChatCompletionRequest & req) { handle_request(req); });

    if (!success) {
        return false;
    }

    running_ = true;
    return true;
}

void DDSBridge::stop() {
    running_ = false;
    if (pimpl_) {
        pimpl_->stop();
    }
}

void DDSBridge::update_status(const std::string & server_id,
                              int32_t             slots_idle,
                              int32_t             slots_processing,
                              const std::string & model_loaded,
                              bool                ready) {
    if (pimpl_) {
        pimpl_->update_status(server_id, slots_idle, slots_processing, model_loaded, ready);
    }
}

void DDSBridge::set_task_complete_callback(TaskCompleteCallback callback) {
    task_complete_callback_ = std::move(callback);
}

void DDSBridge::send_response(const ChatCompletionResponse & response) {
    if (pimpl_) {
        // BRIDGE-STATE-013: decrementa só na PRIMEIRA transição terminal do
        // request_id — um final duplicado não libera capacidade de outra
        // request. O slot é liberado mesmo se o write falhar (a request
        // terminou do ponto de vista do servidor; o cliente expira por
        // timeout e a falha fica observável no log abaixo).
        if (response.is_final && mark_terminal(response.request_id)) {
            pimpl_->dec_pending();
        }
        if (!pimpl_->send_response(response)) {
            fprintf(stderr, "[DDSBridge] dds_write falhou (request=%s, final=%d) — sem reenvio; o consumidor verá timeout/gap\n",
                    response.request_id.c_str(), (int) response.is_final);
        }
    }
}

void DDSBridge::send_error(const LLMInferenceError & error) {
    if (pimpl_) {
        if (mark_terminal(error.request_id)) {
            pimpl_->dec_pending();
        }
        if (!pimpl_->send_error(error)) {
            fprintf(stderr, "[DDSBridge] dds_write de Error falhou (request=%s)\n",
                    error.request_id.c_str());
        }
    }
}

void DDSBridge::cancel_pending_request(const std::string & request_id) {
    if (pimpl_ && mark_terminal(request_id)) {
        pimpl_->dec_pending();
    }
}

int DDSBridge::pending_count() const {
    return pimpl_ ? pimpl_->pending_count() : 0;
}

bool DDSBridge::mark_terminal(const std::string & request_id) {
    static constexpr size_t MAX_TERMINAL_TRACKED = 4096;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!terminal_ids_.insert(request_id).second) {
        return false;  // terminal duplicado — no-op
    }
    terminal_order_.push_back(request_id);
    if (terminal_order_.size() > MAX_TERMINAL_TRACKED) {
        terminal_ids_.erase(terminal_order_.front());
        terminal_order_.pop_front();
    }
    return true;
}

void DDSBridge::set_model_info(const std::string & model_name, bool ready, int n_parallel) {
    if (pimpl_) {
        pimpl_->set_model_info(model_name, ready, n_parallel);
    }
}

void DDSBridge::handle_request(const ChatCompletionRequest & request) {
    // Filtro por modelo: o transporte real (DDSTransport::read_loop, ver
    // dds_transport.cpp) entrega TODO request publicado no domain, sem
    // ContentFilteredTopic nem checagem nenhuma — o isolamento entre
    // instâncias de llama-server hoje é só CONVENÇÃO de deploy (domains
    // distintos por servidor), não é garantido pelo código. Confirmado ao
    // vivo nesta sessão: dois processos `llama-server` rodando no MESMO
    // --dds-domain simultaneamente (achado real via SSH, ver
    // OPTIMIZATION_REPORT.md) — sem este filtro, ambos processariam TODO
    // request do domain em duplicado (trabalho de GPU desperdiçado, sem
    // erro nem aviso). Este filtro também é o mecanismo CORRETO para
    // múltiplos modelos legitimamente compartilhando um domain (cada
    // servidor só atende o `model_name` que carregou). Se `model_loaded_`
    // ainda não foi setado (startup, antes de `set_model_info()`), aceita
    // tudo — preserva o comportamento atual da janela de inicialização.
    // O filtro só age numa DIVERGÊNCIA POSITIVA entre dois nomes conhecidos:
    // se o servidor não tem nome real ("" antes de set_model_info(), ou
    // "unknown" quando roda sem --alias — ver server.cpp: params.model.name
    // vazio vira "unknown"), ou se o request não declara modelo, aceita —
    // senão um deploy sem alias dropava TODO request silenciosamente
    // (pego pelo smoke test HIP local, Rodada 8).
    {
        std::lock_guard<std::mutex> lk(pimpl_->status_mutex_);
        const std::string & loaded = pimpl_->model_loaded_;
        if (!loaded.empty() && loaded != "unknown" && !request.model_name.empty() &&
            loaded != request.model_name) {
#ifdef DDS_DEBUG
            fprintf(stderr, "[DDSBridge] request %s dropped: model=%s != loaded=%s\n",
                    request.request_id.c_str(), request.model_name.c_str(), loaded.c_str());
#endif
            return;  // não é para este servidor — não enfileira, não conta pending
        }
    }

    // Store request for tracking (FIFO via pending_order_, lookup via map).
    // inc_pending() moved INSIDE this lock, gated on `inserted`: the previous
    // version incremented unconditionally before the dedup check below, with
    // a comment justifying that ordering as avoiding a race against a fast
    // pop+complete. That race is real, but incrementing unconditionally
    // introduced a different bug — a duplicate request_id (re-delivery from
    // TRANSIENT_LOCAL, the `else` branch below) got counted again with no
    // matching decrement (dec_pending() only fires once, when the request
    // eventually completes), so pending_count_ drifts upward by one on every
    // redelivery a long-running server sees, corrupting the
    // slots_processing/slots_idle telemetry in ServerStatus over time.
    // Incrementing here, under the SAME lock that performs the insertion,
    // fixes both: it's atomic with the dedup check (no separate race window
    // — a concurrent pop_pending_request(), which also takes `mutex_`, can
    // only observe insertion+increment as one indivisible step, never one
    // without the other), and it only fires for genuinely new requests.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [it, inserted] = pending_requests_.emplace(request.request_id, request);
        if (inserted) {
            pending_order_.push_back(request.request_id);
            // Retry legítimo (mesmo request_id após terminal): rearma a
            // transição terminal para que o novo final decremente o pending
            // desta nova tentativa (BRIDGE-STATE-013).
            terminal_ids_.erase(request.request_id);
            pimpl_->inc_pending();
        } else {
            // Duplicate request_id (re-delivery from TRANSIENT_LOCAL);
            // keep newest payload but don't re-queue, don't re-count.
            it->second = request;
        }
    }

#ifdef DDS_DEBUG
    fprintf(stderr, "[DDSBridge] request queued: model=%s, request_id=%s\n", request.model_name.c_str(),
            request.request_id.c_str());
#endif

    cv_pending_.notify_one();  // wake the server poll loop
}

bool DDSBridge::pop_pending_request(ChatCompletionRequest & out_request) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pending_order_.empty()) {
        auto id = std::move(pending_order_.front());
        pending_order_.pop_front();
        auto it = pending_requests_.find(id);
        if (it == pending_requests_.end()) {
            continue;  // stale id (e.g., cancelled) — try next
        }
        out_request = std::move(it->second);
        pending_requests_.erase(it);
        return true;
    }
    return false;
}

bool DDSBridge::has_pending_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !pending_requests_.empty();
}

bool DDSBridge::wait_for_request(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mutex_);
    cv_pending_.wait_for(lk, timeout, [this] { return !pending_order_.empty() || !running_.load(); });
    return true;  // caller re-checks has_pending_requests()
}

}  // namespace llama_dds
