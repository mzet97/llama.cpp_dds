#pragma once

#include "grpc_transport.h"

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace llama_grpc {

// Callback for when a task completes (mirrors llama_dds::TaskCompleteCallback)
using TaskCompleteCallback = std::function<void(const std::string &                request_id,
                                                const std::string &                content,
                                                const std::optional<std::string> & finish_reason,
                                                bool                               is_final,
                                                int32_t                            prompt_tokens,
                                                int32_t                            completion_tokens)>;

// Forward declaration
class GRPCBridgeImpl;

/// Adapter between the gRPC transport layer and the llama.cpp server loop.
///
/// API mirrors llama_dds::DDSBridge exactly for fair protocol comparison.
///
/// Threading model
/// ---------------
/// The bridge owns two threads:
///   1. gRPC server thread — handles incoming RPCs via GRPCTransport.
///      handle_request() is internal; never call it directly.
///   2. Status-publishing worker thread — periodically caches a ServerStatus
///      snapshot for the GetStatus RPC.
///
/// The server's main loop communicates with the bridge through three
/// thread-safe polling helpers: pop_pending_request(), wait_for_request(),
/// and has_pending_requests().  No external locking is required.
///
/// Lifecycle
/// ---------
///   GRPCBridge bridge("0.0.0.0:50051");
///   bridge.init();
///   bridge.set_model_info(model_name, /*ready=*/true, n_parallel);
///   bridge.start();
///   while (running) {
///       bridge.wait_for_request(100ms);
///       ChatCompletionRequest req;
///       if (bridge.pop_pending_request(req)) { /* process */ }
///   }
///   bridge.stop();
class GRPCBridge {
  public:
    GRPCBridge(const std::string & address = "0.0.0.0:50051");
    ~GRPCBridge();

    // Initialize the gRPC bridge
    bool init();

    // Set callback for processing requests (kept for API symmetry with DDSBridge)
    using ProcessRequestCallback = std::function<void(const llama_dds::ChatCompletionRequest &)>;
    [[deprecated("requests are queued via handle_request; no callback needed")]]
    void set_process_callback(ProcessRequestCallback callback);

    // Start listening for gRPC requests
    bool start();

    // Stop the gRPC bridge
    void stop();

    /// Set model info used by the periodic status-publishing thread.
    void set_model_info(const std::string & model_name, bool ready, int n_parallel = 1);

    // Update server status
    void update_status(const std::string & server_id,
                       int32_t             slots_idle,
                       int32_t             slots_processing,
                       const std::string & model_loaded,
                       bool                ready);

    // Check if gRPC bridge is running
    bool is_running() const { return running_.load(); }

    // Get listen address
    const std::string & get_address() const { return address_; }

    // Set callback for task completion
    void set_task_complete_callback(TaskCompleteCallback callback);

    // Send a response (called by server after processing)
    void send_response(const llama_dds::ChatCompletionResponse & response);

    /// Pop one request from the pending queue.
    /// Thread-safe: may be called from any thread.
    bool pop_pending_request(llama_dds::ChatCompletionRequest & out_request);

    /// Block the calling thread until at least one request is queued or @p timeout expires.
    bool wait_for_request(std::chrono::milliseconds timeout);

    /// Returns true if at least one request is pending. Thread-safe.
    bool has_pending_requests() const;

    /// Decrement pending count for a request that failed before send_response(is_final=true).
    void cancel_pending_request();

  private:
    void handle_request(const llama_dds::ChatCompletionRequest & request);

    std::string address_;

    std::atomic<bool> running_{ false };
    std::atomic<bool> initialized_{ false };

    TaskCompleteCallback   task_complete_callback_;
    ProcessRequestCallback process_callback_;

    mutable std::mutex                                              mutex_;
    std::condition_variable                                         cv_pending_;
    std::map<std::string, llama_dds::ChatCompletionRequest>        pending_requests_;

    // pimpl_ MUST be declared LAST: its destructor joins threads that
    // access mutex_, cv_pending_, and pending_requests_ above.
    std::unique_ptr<GRPCBridgeImpl> pimpl_;
};

}  // namespace llama_grpc
