#pragma once

#include "dds_transport.h"

#include <memory>
#include <string>
#include <functional>
#include <map>
#include <mutex>

namespace llama_dds {

// Callback for when a task completes
using TaskCompleteCallback = std::function<void(
    const std::string& request_id,
    const std::string& content,
    const std::optional<std::string>& finish_reason,
    bool is_final,
    int32_t prompt_tokens,
    int32_t completion_tokens
)>;

// Forward declaration
class DDSBridgeImpl;

class DDSBridge {
public:
    DDSBridge(int domain_id = 0);
    ~DDSBridge();

    // Initialize the DDS bridge (server context will be passed via callback)
    bool init();

    // Set callback for processing requests (called from server)
    using ProcessRequestCallback = std::function<void(const ChatCompletionRequest&)>;
    void set_process_callback(ProcessRequestCallback callback);

    // Start listening for DDS requests
    bool start();

    // Stop the DDS bridge
    void stop();

    // Update server status (call periodically or on state change)
    void update_status(
        const std::string& server_id,
        int32_t slots_idle,
        int32_t slots_processing,
        const std::string& model_loaded,
        bool ready
    );

    // Check if DDS bridge is running
    bool is_running() const { return running_.load(); }

    // Get domain ID
    int get_domain_id() const { return domain_id_; }

    // Set callback for task completion
    void set_task_complete_callback(TaskCompleteCallback callback);

    // Send a response (called by server after processing)
    void send_response(const ChatCompletionResponse& response);

    // Get pending requests (called from server main loop to avoid threading issues)
    // Returns true if a request was popped
    bool pop_pending_request(ChatCompletionRequest& out_request);

    // Check if there are pending requests
    bool has_pending_requests() const;

private:
    void handle_request(const ChatCompletionRequest& request);

    int domain_id_;
    std::unique_ptr<DDSBridgeImpl> pimpl_;

    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};

    TaskCompleteCallback task_complete_callback_;
    ProcessRequestCallback process_callback_;

    mutable std::mutex mutex_;
    std::map<std::string, ChatCompletionRequest> pending_requests_;
};

} // namespace llama_dds
