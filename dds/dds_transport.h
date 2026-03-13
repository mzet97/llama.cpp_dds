#pragma once

#include "dds_types.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace llama_dds {

// Forward declarations
class DDSTransportImpl;

/// Low-level DDS send/receive layer for the llama.cpp inference server.
///
/// Threading model
/// ---------------
/// - start_server() / start_client() spawn internal threads (reader loop).
/// - send_response(), publish_status(), and send_request() are safe to call
///   from any thread after the respective start_*() call succeeds.
/// - subscribe_responses() and subscribe_status() MUST be called before
///   start_client(); the callbacks are invoked from the internal reader thread.
/// - stop_server() / stop_client() block until the reader thread exits.
///   Do not call them from inside a callback.
///
/// Typical server usage
/// --------------------
///   DDSTransport t;
///   t.start_server([&](const ChatCompletionRequest& r) { /* enqueue r */ });
///   // ... later, from any thread:
///   t.send_response(resp);
///   t.stop_server();
class DDSTransport {
  public:
    using RequestCallback  = std::function<void(const ChatCompletionRequest &)>;
    using ResponseCallback = std::function<void(const ChatCompletionResponse &)>;
    using StatusCallback   = std::function<void(const ServerStatus &)>;

    DDSTransport(int domain_id = 0);
    ~DDSTransport();

    // Prevent copying
    DDSTransport(const DDSTransport &)             = delete;
    DDSTransport & operator=(const DDSTransport &) = delete;

    // Moving not supported (std::atomic is not movable)
    DDSTransport(DDSTransport &&) = delete;
    DDSTransport & operator=(DDSTransport &&) = delete;

    /// @name Server-mode interface
    /// @{

    /// Begin listening for inbound requests.
    /// Spawns an internal reader thread; @p on_request is invoked from that thread.
    bool start_server(RequestCallback on_request);

    /// Stop the reader thread and release all DDS entities.
    void stop_server();

    /// Publish @p response on the response topic. Thread-safe.
    void send_response(const ChatCompletionResponse & response);

    /// Publish a server heartbeat on the status topic. Thread-safe.
    void publish_status(const ServerStatus & status);

    /// @}
    /// @name Client-mode interface
    /// @{

    /// Begin listening for inbound responses.
    /// Call subscribe_responses() and subscribe_status() before this.
    bool start_client();

    /// Stop the response reader thread and release all DDS entities.
    void stop_client();

    /// Publish @p request on the request topic. Thread-safe.
    void send_request(const ChatCompletionRequest & request);

    /// Register a callback invoked for each received response.
    /// MUST be called before start_client(); start_client() returns false if omitted.
    void subscribe_responses(ResponseCallback on_response);

    /// Register a callback invoked for each received server-status update.
    /// Should be called before start_client() (optional — status updates are
    /// silently ignored if no callback is registered).
    void subscribe_status(StatusCallback on_status);

    /// @}

    /// Returns true while the reader thread is active.
    bool is_running() const { return running_.load(); }

    int get_domain_id() const { return domain_id_; }

  private:
    int                               domain_id_;
    std::unique_ptr<DDSTransportImpl> pimpl_;
    std::atomic<bool>                 running_{ false };
    std::atomic<bool>                 is_server_{ false };

    RequestCallback  request_callback_;
    ResponseCallback response_callback_;
    StatusCallback   status_callback_;
};

}  // namespace llama_dds
