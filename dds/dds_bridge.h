#pragma once

#include "dds_transport.h"

#include <condition_variable>
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

/// Adapter between the DDS transport layer and the llama.cpp server loop.
///
/// Threading model
/// ---------------
/// The bridge owns two threads:
///   1. DDSTransport reader thread — calls handle_request() when a DDS message
///      arrives.  handle_request() is internal; never call it directly.
///   2. Status-publishing worker thread — periodically writes a ServerStatus
///      heartbeat to the DDS domain.
///
/// The server’s main loop communicates with the bridge through three
/// thread-safe polling helpers: pop_pending_request(), wait_for_request(),
/// and has_pending_requests().  No external locking is required.
///
/// Lifecycle
/// ---------
///   DDSBridge bridge;
///   bridge.init();
///   bridge.set_model_info(model_name, /*ready=*/true, n_parallel);
///   bridge.start();
///   while (running) {
///       bridge.wait_for_request(100ms);
///       ChatCompletionRequest req;
///       if (bridge.pop_pending_request(req)) { /* process */ }
///   }
///   bridge.stop();
class DDSBridge {
  public:
    DDSBridge(int domain_id = 0);
    ~DDSBridge();

    // Initialize the DDS bridge (server context will be passed via callback)
    bool init();

    // Set callback for processing requests (called from server)
    using ProcessRequestCallback = std::function<void(const ChatCompletionRequest &)>;
    [[deprecated("requests are queued via handle_request; no callback needed")]] void set_process_callback(
        ProcessRequestCallback callback);

    // Start listening for DDS requests
    bool start();

    // Stop the DDS bridge
    void stop();

    /// Set model info used by the periodic status-publishing thread.
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

    /// Pop one request from the pending queue.
    /// Thread-safe: may be called from any thread concurrently with handle_request().
    /// Returns true and fills @p out_request when a request is available;
    /// returns false immediately when the queue is empty.
    bool pop_pending_request(ChatCompletionRequest & out_request);

    /// Block the calling thread until at least one request is queued or @p timeout
    /// expires.  Spurious wake-ups are possible; always re-check has_pending_requests().
    /// Thread-safe: may be called from the server main loop while the DDS reader
    /// thread enqueues requests.
    bool wait_for_request(std::chrono::milliseconds timeout);

    /// Returns true if at least one request is pending.  Thread-safe.
    bool has_pending_requests() const;

  private:
    void handle_request(const ChatCompletionRequest & request);

    int                            domain_id_;
    std::unique_ptr<DDSBridgeImpl> pimpl_;

    std::atomic<bool> running_{ false };
    std::atomic<bool> initialized_{ false };

    TaskCompleteCallback   task_complete_callback_;
    // Kept for ABI compatibility with set_process_callback(); has no effect.
    ProcessRequestCallback process_callback_;

    mutable std::mutex                           mutex_;
    std::condition_variable                      cv_pending_;  // notified by handle_request() on every enqueue
    std::map<std::string, ChatCompletionRequest> pending_requests_;
};

}  // namespace llama_dds
