#pragma once

#include "dds_types.h"

#include <functional>
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace llama_dds {

// Forward declarations
class DDSTransportImpl;

class DDSTransport {
public:
    using RequestCallback = std::function<void(const ChatCompletionRequest&)>;
    using ResponseCallback = std::function<void(const ChatCompletionResponse&)>;
    using StatusCallback = std::function<void(const ServerStatus&)>;

    DDSTransport(int domain_id = 0);
    ~DDSTransport();

    // Prevent copying
    DDSTransport(const DDSTransport&) = delete;
    DDSTransport& operator=(const DDSTransport&) = delete;

    // Allow moving
    DDSTransport(DDSTransport&&) noexcept;
    DDSTransport& operator=(DDSTransport&&) noexcept;

    // Server mode: listen for requests and publish responses
    bool start_server(RequestCallback on_request);
    void stop_server();

    // Publish response to a request
    void send_response(const ChatCompletionResponse& response);

    // Publish server status
    void publish_status(const ServerStatus& status);

    // Client mode: send requests and receive responses
    bool start_client();
    void stop_client();

    // Send request (client mode)
    void send_request(const ChatCompletionRequest& request);

    // Subscribe to responses (client mode)
    void subscribe_responses(ResponseCallback on_response);

    // Subscribe to server status updates
    void subscribe_status(StatusCallback on_status);

    // Check if transport is running
    bool is_running() const { return running_.load(); }

    // Get domain ID
    int get_domain_id() const { return domain_id_; }

private:
    int domain_id_;
    std::unique_ptr<DDSTransportImpl> pimpl_;
    std::atomic<bool> running_{false};
    std::atomic<bool> is_server_{false};

    RequestCallback request_callback_;
    ResponseCallback response_callback_;
    StatusCallback status_callback_;
};

} // namespace llama_dds
