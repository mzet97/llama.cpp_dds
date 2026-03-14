#pragma once

#include "dds_types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace llama_grpc {

// Forward declaration
class GRPCTransportImpl;

/// Low-level gRPC send/receive layer for the llama.cpp inference server.
///
/// API mirrors llama_dds::DDSTransport exactly to enable fair protocol comparison.
///
/// Threading model
/// ---------------
/// - start_server() spawns a gRPC server thread (grpc::Server).
/// - start_client() creates a gRPC channel + stub.
/// - send_response(), publish_status(), and send_request() are safe to call
///   from any thread after the respective start_*() call succeeds.
/// - subscribe_responses() and subscribe_status() MUST be called before
///   start_client(); the callbacks are invoked from an internal reader thread.
/// - stop_server() / stop_client() block until threads exit.
///   Do not call them from inside a callback.
///
/// Typical server usage
/// --------------------
///   GRPCTransport t("0.0.0.0:50051");
///   t.start_server([&](const ChatCompletionRequest& r) { /* enqueue r */ });
///   // ... later, from any thread:
///   t.send_response(resp);
///   t.stop_server();
class GRPCTransport {
  public:
    using RequestCallback  = std::function<void(const llama_dds::ChatCompletionRequest &)>;
    using ResponseCallback = std::function<void(const llama_dds::ChatCompletionResponse &)>;
    using StatusCallback   = std::function<void(const llama_dds::ServerStatus &)>;

    GRPCTransport(const std::string & address = "0.0.0.0:50051");
    ~GRPCTransport();

    // Prevent copying and moving
    GRPCTransport(const GRPCTransport &)            = delete;
    GRPCTransport & operator=(const GRPCTransport &) = delete;
    GRPCTransport(GRPCTransport &&)                  = delete;
    GRPCTransport & operator=(GRPCTransport &&)      = delete;

    /// @name Server-mode interface
    /// @{

    /// Start gRPC server, listening for inbound requests.
    /// @p on_request is invoked from the gRPC server thread for each incoming RPC.
    bool start_server(RequestCallback on_request);

    /// Stop the gRPC server and release all resources.
    void stop_server();

    /// Send @p response back to the client. Thread-safe.
    /// For streaming RPCs, intermediate chunks have is_final=false;
    /// the final chunk has is_final=true and closes the stream.
    void send_response(const llama_dds::ChatCompletionResponse & response);

    /// Update the cached server status returned by GetStatus RPC. Thread-safe.
    void publish_status(const llama_dds::ServerStatus & status);

    /// @}
    /// @name Client-mode interface
    /// @{

    /// Create gRPC channel and stub to the server.
    /// Call subscribe_responses() and subscribe_status() before this.
    bool start_client();

    /// Shutdown the client channel.
    void stop_client();

    /// Send a chat completion request to the server. Thread-safe.
    /// Invokes subscribe_responses callback with each response chunk.
    void send_request(const llama_dds::ChatCompletionRequest & request);

    /// Register a callback invoked for each received response.
    /// MUST be called before start_client().
    void subscribe_responses(ResponseCallback on_response);

    /// Register a callback invoked for server-status updates.
    /// Should be called before start_client() (optional).
    void subscribe_status(StatusCallback on_status);

    /// @}

    /// Returns true while the server/client is active.
    bool is_running() const { return running_.load(); }

    const std::string & get_address() const { return address_; }

  private:
    std::string                       address_;
    std::unique_ptr<GRPCTransportImpl> pimpl_;
    std::atomic<bool>                 running_{ false };
    std::atomic<bool>                 is_server_{ false };

    RequestCallback  request_callback_;
    ResponseCallback response_callback_;
    StatusCallback   status_callback_;
};

}  // namespace llama_grpc
