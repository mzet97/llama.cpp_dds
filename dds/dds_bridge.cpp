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

        // Start status publishing thread
        worker_thread_ = std::thread([this]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::seconds(5));

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
        ServerStatus status;
        status.server_id        = server_id;
        status.slots_idle       = slots_idle;
        status.slots_processing = slots_processing;
        status.model_loaded     = model_loaded;
        status.ready            = ready;

        transport_->publish_status(status);
    }

    void send_response(const ChatCompletionResponse & response) { transport_->send_response(response); }

    // Update model name, readiness, and parallel-slot capacity for status publishing.
    void set_model_info(const std::string & model_name, bool ready, int n_parallel = 1) {
        std::lock_guard<std::mutex> lk(status_mutex_);
        model_loaded_ = model_name;
        model_ready_  = ready;
        total_slots_  = std::max(1, n_parallel);
    }

    // Increment/decrement the count of in-flight requests, used for status reporting.
    void inc_pending() { pending_count_.fetch_add(1); }

    void dec_pending() {
        if (pending_count_.load() > 0) {
            pending_count_.fetch_sub(1);
        }
    }

    std::unique_ptr<DDSTransport> transport_;

    std::atomic<bool> running_{ false };
    std::atomic<int>  pending_count_{ 0 };  // number of in-flight requests
    int               domain_id_;

    // Model state snapshot for the status-publishing thread.
    mutable std::mutex status_mutex_;
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
        pimpl_->dec_pending();  // one less in-flight request
        pimpl_->send_response(response);
    }
}

void DDSBridge::set_model_info(const std::string & model_name, bool ready, int n_parallel) {
    if (pimpl_) {
        pimpl_->set_model_info(model_name, ready, n_parallel);
    }
}

void DDSBridge::handle_request(const ChatCompletionRequest & request) {
    // Store request for tracking
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_requests_[request.request_id] = request;
    }

    pimpl_->inc_pending();  // track in-flight count for status reporting

    fprintf(stderr, "[DDSBridge] request queued: model=%s, request_id=%s\n", request.model.c_str(),
            request.request_id.c_str());

    cv_pending_.notify_one();  // wake the server poll loop
}

bool DDSBridge::pop_pending_request(ChatCompletionRequest & out_request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_requests_.empty()) {
        return false;
    }
    auto it     = pending_requests_.begin();
    out_request = it->second;
    pending_requests_.erase(it);
    return true;
}

bool DDSBridge::has_pending_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !pending_requests_.empty();
}

bool DDSBridge::wait_for_request(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mutex_);
    cv_pending_.wait_for(lk, timeout, [this] { return !pending_requests_.empty() || !running_.load(); });
    return true;  // caller re-checks has_pending_requests()
}

}  // namespace llama_dds
