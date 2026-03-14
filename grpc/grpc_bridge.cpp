#include "grpc_bridge.h"

#include "grpc_transport.h"

#include <chrono>
#include <condition_variable>
#include <thread>

namespace llama_grpc {

class GRPCBridgeImpl {
  public:
    GRPCBridgeImpl(const std::string & address) :
        address_(address),
        transport_(std::make_unique<GRPCTransport>(address)) {}

    ~GRPCBridgeImpl() { stop(); }

    bool start(GRPCBridge::ProcessRequestCallback on_request) {
        bool success = transport_->start_server(std::move(on_request));

        if (!success) {
            fprintf(stderr, "[GRPCBridge] failed to start gRPC transport\n");
            return false;
        }

        running_ = true;

        // Start status publishing thread (mirrors DDSBridgeImpl)
        worker_thread_ = std::thread([this]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::seconds(5));

                if (!running_) {
                    break;
                }

                llama_dds::ServerStatus status;
                status.server_id = "llama-grpc-server";
                {
                    std::lock_guard<std::mutex> lk(status_mutex_);
                    status.slots_processing = (int32_t) pending_count_.load();
                    status.slots_idle       = std::max(0, total_slots_ - status.slots_processing);
                    status.model_loaded     = model_loaded_;
                    status.ready            = model_ready_;
                }

                transport_->publish_status(status);
            }
        });

        fprintf(stderr, "[GRPCBridge] started successfully on %s\n", address_.c_str());
        return true;
    }

    void stop() {
        running_ = false;

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
        llama_dds::ServerStatus status;
        status.server_id        = server_id;
        status.slots_idle       = slots_idle;
        status.slots_processing = slots_processing;
        status.model_loaded     = model_loaded;
        status.ready            = ready;

        transport_->publish_status(status);
    }

    void send_response(const llama_dds::ChatCompletionResponse & response) {
        transport_->send_response(response);
    }

    void set_model_info(const std::string & model_name, bool ready, int n_parallel = 1) {
        std::lock_guard<std::mutex> lk(status_mutex_);
        model_loaded_ = model_name;
        model_ready_  = ready;
        total_slots_  = std::max(1, n_parallel);
    }

    void inc_pending() { pending_count_.fetch_add(1); }

    void dec_pending() {
        int old = pending_count_.load();
        while (old > 0 && !pending_count_.compare_exchange_weak(old, old - 1)) {
            // CAS loop: retry if another thread modified pending_count_
        }
    }

    std::unique_ptr<GRPCTransport> transport_;

    std::atomic<bool> running_{ false };
    std::atomic<int>  pending_count_{ 0 };
    std::string       address_;

    mutable std::mutex status_mutex_;
    std::string        model_loaded_;
    bool               model_ready_{ false };
    int                total_slots_{ 1 };

    std::thread worker_thread_;
};

// GRPCBridge implementation
GRPCBridge::GRPCBridge(const std::string & address) :
    address_(address),
    pimpl_(std::make_unique<GRPCBridgeImpl>(address)) {}

GRPCBridge::~GRPCBridge() = default;

bool GRPCBridge::init() {
    initialized_ = true;
    return true;
}

void GRPCBridge::set_process_callback(ProcessRequestCallback callback) {
    process_callback_ = std::move(callback);
}

bool GRPCBridge::start() {
    if (!initialized_) {
        fprintf(stderr, "[GRPCBridge] not initialized\n");
        return false;
    }

    bool success = pimpl_->start([this](const llama_dds::ChatCompletionRequest & req) { handle_request(req); });

    if (!success) {
        return false;
    }

    running_ = true;
    return true;
}

void GRPCBridge::stop() {
    running_ = false;
    if (pimpl_) {
        pimpl_->stop();
    }
}

void GRPCBridge::update_status(const std::string & server_id,
                               int32_t             slots_idle,
                               int32_t             slots_processing,
                               const std::string & model_loaded,
                               bool                ready) {
    if (pimpl_) {
        pimpl_->update_status(server_id, slots_idle, slots_processing, model_loaded, ready);
    }
}

void GRPCBridge::set_task_complete_callback(TaskCompleteCallback callback) {
    task_complete_callback_ = std::move(callback);
}

void GRPCBridge::send_response(const llama_dds::ChatCompletionResponse & response) {
    if (pimpl_) {
        if (response.is_final) {
            pimpl_->dec_pending();
        }
        pimpl_->send_response(response);
    }
}

void GRPCBridge::cancel_pending_request() {
    if (pimpl_) {
        pimpl_->dec_pending();
    }
}

void GRPCBridge::set_model_info(const std::string & model_name, bool ready, int n_parallel) {
    if (pimpl_) {
        pimpl_->set_model_info(model_name, ready, n_parallel);
    }
}

void GRPCBridge::handle_request(const llama_dds::ChatCompletionRequest & request) {
    pimpl_->inc_pending();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_requests_[request.request_id] = request;
    }

    fprintf(stderr, "[GRPCBridge] request queued: model=%s, request_id=%s\n",
            request.model.c_str(), request.request_id.c_str());

    cv_pending_.notify_one();
}

bool GRPCBridge::pop_pending_request(llama_dds::ChatCompletionRequest & out_request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_requests_.empty()) {
        return false;
    }
    auto it     = pending_requests_.begin();
    out_request = it->second;
    pending_requests_.erase(it);
    return true;
}

bool GRPCBridge::has_pending_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !pending_requests_.empty();
}

bool GRPCBridge::wait_for_request(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mutex_);
    cv_pending_.wait_for(lk, timeout, [this] { return !pending_requests_.empty() || !running_.load(); });
    return true;
}

}  // namespace llama_grpc
