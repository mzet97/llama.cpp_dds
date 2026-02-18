#pragma once

#include "dds_transport.h"

#include <condition_variable>  // A4
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace llama_dds {

// Callback for when a task completes
using TaskCompleteCallback = std::function<void(const std::string &                request_id,
                                                const std::string &                content,
                                                const std::optional<std::string> & finish_reason,
                                                bool                               is_final,
                                                int32_t                            prompt_tokens,
                                                int32_t                            completion_tokens)>;

// Forward declaration
class DDSBridgeImpl;

class DDSBridge {
  public:
    DDSBridge(int domain_id = 0);
    ~DDSBridge();

    // Initialize the DDS bridge (server context will be passed via callback)
    bool init();

    // Set callback for processing requests (called from server)
    using ProcessRequestCallback = std::function<void(const ChatCompletionRequest &)>;
    // M2: Deprecated — requests are now routed directly through handle_request().
    //     Kept for ABI compatibility but has no effect.
    [[deprecated("requests are queued via handle_request; no callback needed")]] void set_process_callback(
        ProcessRequestCallback callback);

    // Start listening for DDS requests
    bool start();

    // Stop the DDS bridge
    void stop();

    // A3: set model info used by periodic status publishing
    void set_model_info(const std::string & model_name, bool ready, int n_parallel = 1);

    // Update server status (call periodically or on state change)
    void update_status(const std::string & server_id,
                       int32_t             slots_idle,
                       int32_t             slots_processing,
                       const std::string & model_loaded,
                       bool                ready);

    // Check if DDS bridge is running
    bool is_running() const { return running_.load(); }

    // Get domain ID
    int get_domain_id() const { return domain_id_; }

    // Set callback for task completion
    void set_task_complete_callback(TaskCompleteCallback callback);

    // Send a response (called by server after processing)
    void send_response(const ChatCompletionResponse & response);

    // Get pending requests (called from server main loop to avoid threading issues)
    // Returns true if a request was popped
    bool pop_pending_request(ChatCompletionRequest & out_request);

    // A4: Block until at least one request arrives or timeout expires.
    // Returns true if a request may be available (spurious wake-ups are OK).
    bool wait_for_request(std::chrono::milliseconds timeout);

    // Check if there are pending requests
    bool has_pending_requests() const;

  private:
    void handle_request(const ChatCompletionRequest & request);

    int                            domain_id_;
    std::unique_ptr<DDSBridgeImpl> pimpl_;

    std::atomic<bool> running_{ false };
    std::atomic<bool> initialized_{ false };

    TaskCompleteCallback   task_complete_callback_;
    // M2: process_callback_ no longer used — kept for ABI compat only
    ProcessRequestCallback process_callback_;

    mutable std::mutex                           mutex_;
    std::condition_variable                      cv_pending_;  // A4: notified when a request arrives
    std::map<std::string, ChatCompletionRequest> pending_requests_;
};

}  // namespace llama_dds
