#include "dds_bridge.h"

#include "dds_transport.h"

#include <thread>
#include <chrono>

namespace llama_dds {

class DDSBridgeImpl {
public:
    DDSBridgeImpl(int domain_id)
        : domain_id_(domain_id)
        , transport_(std::make_unique<DDSTransport>(domain_id)) {}

    ~DDSBridgeImpl() {
        stop();
    }

    bool start(DDSBridge::ProcessRequestCallback on_request) {
        process_callback_ = std::move(on_request);

        bool success = transport_->start_server([this](const ChatCompletionRequest& req) {
            handle_request(req);
        });

        if (!success) {
            fprintf(stderr, "[DDSBridge] failed to start DDS transport\n");
            return false;
        }

        running_ = true;

        // Start status publishing thread
        worker_thread_ = std::thread([this]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::seconds(5));

                if (!running_) break;

                // Publish status every 5 seconds
                ServerStatus status;
                status.server_id = "llama-dds-server";
                status.slots_idle = 0;
                status.slots_processing = 0;
                status.model_loaded = "";
                status.ready = true;

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

    void update_status(
        const std::string& server_id,
        int32_t slots_idle,
        int32_t slots_processing,
        const std::string& model_loaded,
        bool ready) {

        ServerStatus status;
        status.server_id = server_id;
        status.slots_idle = slots_idle;
        status.slots_processing = slots_processing;
        status.model_loaded = model_loaded;
        status.ready = ready;

        transport_->publish_status(status);
    }

    void send_response(const ChatCompletionResponse& response) {
        transport_->send_response(response);
    }

    void handle_request(const ChatCompletionRequest& req) {
        if (process_callback_) {
            process_callback_(req);
        } else {
            // No callback, send error response
            ChatCompletionResponse error_resp;
            error_resp.request_id = req.request_id;
            error_resp.model = req.model;
            error_resp.content = "Error: No processing callback registered";
            error_resp.is_final = true;
            error_resp.finish_reason = "error";
            transport_->send_response(error_resp);
        }
    }

    std::unique_ptr<DDSTransport> transport_;

    std::atomic<bool> running_{false};
    int domain_id_;

    DDSBridge::ProcessRequestCallback process_callback_;
    std::thread worker_thread_;
};

// DDSBridge implementation
DDSBridge::DDSBridge(int domain_id)
    : domain_id_(domain_id)
    , pimpl_(std::make_unique<DDSBridgeImpl>(domain_id)) {}

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

    bool success = pimpl_->start([this](const ChatCompletionRequest& req) {
        handle_request(req);
    });

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

void DDSBridge::update_status(
    const std::string& server_id,
    int32_t slots_idle,
    int32_t slots_processing,
    const std::string& model_loaded,
    bool ready) {

    if (pimpl_) {
        pimpl_->update_status(server_id, slots_idle, slots_processing, model_loaded, ready);
    }
}

void DDSBridge::set_task_complete_callback(TaskCompleteCallback callback) {
    task_complete_callback_ = std::move(callback);
}

void DDSBridge::send_response(const ChatCompletionResponse& response) {
    if (pimpl_) {
        pimpl_->send_response(response);
    }
}

void DDSBridge::handle_request(const ChatCompletionRequest& request) {
    // Store request for tracking
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_requests_[request.request_id] = request;
    }

    if (process_callback_) {
        process_callback_(request);
    }
}

bool DDSBridge::pop_pending_request(ChatCompletionRequest& out_request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_requests_.empty()) {
        return false;
    }
    auto it = pending_requests_.begin();
    out_request = it->second;
    pending_requests_.erase(it);
    return true;
}

bool DDSBridge::has_pending_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !pending_requests_.empty();
}

} // namespace llama_dds
