#include "grpc_transport.h"

#include "grpc_proto_wrapper.h"
#include "llama_service.grpc.pb.h"
#include "llama_service.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace llama_grpc {

// ═══════════════════════════════════════════════════════════════════════
// Per-stream state: holds the ServerWriter* for an in-flight streaming RPC
// so that send_response() can route chunks to the correct client stream.
// ═══════════════════════════════════════════════════════════════════════
struct StreamState {
    grpc::ServerWriter<llama_grpc::ChatCompletionResponse> * writer = nullptr;
    std::mutex                                                mtx;
    std::condition_variable                                   cv;
    bool                                                      done = false;
};

// ═══════════════════════════════════════════════════════════════════════
// gRPC Service Implementation (server-side)
// ═══════════════════════════════════════════════════════════════════════
class LlamaServiceImpl final : public llama_grpc::LlamaService::Service {
  public:
    using RequestCallback = GRPCTransport::RequestCallback;

    explicit LlamaServiceImpl(RequestCallback on_request) : on_request_(std::move(on_request)) {}

    // Unary Chat — waits for the final response then returns it.
    grpc::Status Chat(grpc::ServerContext *                          context,
                      const llama_grpc::ChatCompletionRequest *      request,
                      llama_grpc::ChatCompletionResponse *           response) override {
        (void) context;

        auto cpp_req       = from_proto(*request);
        cpp_req.stream     = false;  // force non-streaming for unary RPC

        // Create a stream state to collect the final response
        auto state = std::make_shared<StreamState>();
        {
            std::lock_guard<std::mutex> lk(streams_mutex_);
            active_streams_[cpp_req.request_id] = state;
        }

        // Dispatch to the server processing pipeline
        if (on_request_) {
            on_request_(cpp_req);
        }

        // Block until the response is ready (send_response sets done=true on is_final)
        {
            std::unique_lock<std::mutex> lk(state->mtx);
            state->cv.wait(lk, [&] { return state->done; });
        }

        // Collect the final response
        {
            std::lock_guard<std::mutex> lk(streams_mutex_);
            active_streams_.erase(cpp_req.request_id);
        }

        // The response was already written into final_responses_
        {
            std::lock_guard<std::mutex> lk(final_mutex_);
            auto it = final_responses_.find(cpp_req.request_id);
            if (it != final_responses_.end()) {
                *response = std::move(it->second);
                final_responses_.erase(it);
            }
        }

        return grpc::Status::OK;
    }

    // Server-streaming Chat — keeps the stream open until is_final.
    grpc::Status StreamChat(grpc::ServerContext *                                          context,
                            const llama_grpc::ChatCompletionRequest *                      request,
                            grpc::ServerWriter<llama_grpc::ChatCompletionResponse> *       writer) override {
        (void) context;

        auto cpp_req   = from_proto(*request);
        cpp_req.stream = true;

        auto state    = std::make_shared<StreamState>();
        state->writer = writer;
        {
            std::lock_guard<std::mutex> lk(streams_mutex_);
            active_streams_[cpp_req.request_id] = state;
        }

        if (on_request_) {
            on_request_(cpp_req);
        }

        // Block until done — the writer is used by send_response() from another thread
        {
            std::unique_lock<std::mutex> lk(state->mtx);
            state->cv.wait(lk, [&] { return state->done; });
        }

        {
            std::lock_guard<std::mutex> lk(streams_mutex_);
            active_streams_.erase(cpp_req.request_id);
        }

        return grpc::Status::OK;
    }

    // GetStatus — returns cached status snapshot
    grpc::Status GetStatus(grpc::ServerContext *    context,
                           const llama_grpc::Empty * request,
                           llama_grpc::ServerStatus * response) override {
        (void) context;
        (void) request;

        std::lock_guard<std::mutex> lk(status_mutex_);
        *response = cached_status_;
        return grpc::Status::OK;
    }

    // Called by GRPCTransport::send_response() to route a chunk to the correct stream.
    void route_response(const llama_dds::ChatCompletionResponse & resp) {
        auto proto = to_proto(resp);

        std::shared_ptr<StreamState> state;
        {
            std::lock_guard<std::mutex> lk(streams_mutex_);
            auto it = active_streams_.find(resp.request_id);
            if (it == active_streams_.end()) {
                fprintf(stderr, "[gRPC] Warning: no active stream for request_id=%s\n", resp.request_id.c_str());
                return;
            }
            state = it->second;
        }

        bool should_notify = false;

        {
            std::lock_guard<std::mutex> lk(state->mtx);

            if (state->writer) {
                // Streaming RPC: write chunk to the ServerWriter
                state->writer->Write(proto);
            }

            if (resp.is_final) {
                if (!state->writer) {
                    // Unary RPC: store the final response
                    std::lock_guard<std::mutex> flk(final_mutex_);
                    final_responses_[resp.request_id] = std::move(proto);
                }
                state->done    = true;
                should_notify  = true;
            }
        }

        // Notify AFTER releasing state->mtx so waiters can re-acquire immediately
        if (should_notify) {
            state->cv.notify_all();
        }
    }

    // Called by GRPCTransport::publish_status() to cache the latest status
    void update_status(const llama_dds::ServerStatus & status) {
        std::lock_guard<std::mutex> lk(status_mutex_);
        cached_status_ = to_proto(status);
    }

  private:
    RequestCallback on_request_;

    // Active streams indexed by request_id
    std::mutex                                           streams_mutex_;
    std::map<std::string, std::shared_ptr<StreamState>> active_streams_;

    // Cached status for GetStatus RPC
    std::mutex               status_mutex_;
    llama_grpc::ServerStatus cached_status_;

    // For unary RPCs: store the final response until Chat() collects it
    std::mutex                                              final_mutex_;
    std::map<std::string, llama_grpc::ChatCompletionResponse> final_responses_;
};

// ═══════════════════════════════════════════════════════════════════════
// GRPCTransportImpl — pimpl holding the gRPC server or client
// ═══════════════════════════════════════════════════════════════════════
class GRPCTransportImpl {
  public:
    explicit GRPCTransportImpl(const std::string & address) : address_(address) {
        fprintf(stderr, "[gRPC] Transport created with address: %s\n", address.c_str());
    }

    ~GRPCTransportImpl() { stop(); }

    // ─── Server mode ─────────────────────────────────────────────────

    bool start_server(GRPCTransport::RequestCallback on_request) {
        try {
            service_ = std::make_unique<LlamaServiceImpl>(std::move(on_request));

            grpc::ServerBuilder builder;
            builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
            builder.RegisterService(service_.get());
            // Set max message sizes for large prompts
            builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);  // 64 MB
            builder.SetMaxSendMessageSize(64 * 1024 * 1024);

            server_ = builder.BuildAndStart();
            if (!server_) {
                fprintf(stderr, "[gRPC] Failed to start server on %s\n", address_.c_str());
                return false;
            }

            running_.store(true, std::memory_order_release);
            fprintf(stderr, "[gRPC] Server started on %s\n", address_.c_str());
            return true;
        } catch (const std::exception & e) {
            fprintf(stderr, "[gRPC] Exception starting server: %s\n", e.what());
            stop();
            return false;
        }
    }

    void stop() {
        bool was_running = running_.exchange(false);
        if (!was_running) {
            return;
        }

        if (server_) {
            server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
            server_->Wait();
            server_.reset();
        }

        // Join client reader thread if active
        if (client_thread_.joinable()) {
            client_thread_.join();
        }

        fprintf(stderr, "[gRPC] Transport stopped\n");
    }

    void send_response(const llama_dds::ChatCompletionResponse & response) {
        if (!running_.load(std::memory_order_acquire) || !service_) {
            return;
        }
        service_->route_response(response);
        fprintf(stderr, "[gRPC] Sent response for request: %s\n", response.request_id.c_str());
    }

    void publish_status(const llama_dds::ServerStatus & status) {
        if (!running_.load(std::memory_order_acquire) || !service_) {
            return;
        }
        service_->update_status(status);
    }

    // ─── Client mode ─────────────────────────────────────────────────

    bool start_client(GRPCTransport::ResponseCallback on_response,
                      GRPCTransport::StatusCallback   on_status) {
        try {
            auto channel = grpc::CreateChannel(address_, grpc::InsecureChannelCredentials());
            stub_        = llama_grpc::LlamaService::NewStub(channel);

            response_callback_ = std::move(on_response);
            status_callback_   = std::move(on_status);
            running_.store(true, std::memory_order_release);

            fprintf(stderr, "[gRPC Client] connected to %s\n", address_.c_str());
            return true;
        } catch (const std::exception & e) {
            fprintf(stderr, "[gRPC Client] Exception: %s\n", e.what());
            return false;
        }
    }

    void send_request(const llama_dds::ChatCompletionRequest & request) {
        if (!stub_) {
            fprintf(stderr, "[gRPC Client] not started — call start_client() first\n");
            return;
        }

        auto proto_req = to_proto(request);

        // Launch streaming reader in a detached thread for each request
        // (mirrors DDS where responses arrive asynchronously)
        auto stub_ptr          = stub_.get();
        auto response_callback = response_callback_;
        auto request_id        = request.request_id;

        std::thread([stub_ptr, proto_req, response_callback, request_id]() {
            grpc::ClientContext context;
            auto reader = stub_ptr->StreamChat(&context, proto_req);

            llama_grpc::ChatCompletionResponse proto_resp;
            while (reader->Read(&proto_resp)) {
                if (response_callback) {
                    try {
                        response_callback(from_proto(proto_resp));
                    } catch (const std::exception & e) {
                        fprintf(stderr, "[gRPC Client] response callback error: %s\n", e.what());
                    }
                }
            }

            grpc::Status status = reader->Finish();
            if (!status.ok()) {
                fprintf(stderr, "[gRPC Client] StreamChat failed for %s: %s\n",
                        request_id.c_str(), status.error_message().c_str());
            }
        }).detach();

        fprintf(stderr, "[gRPC Client] Request sent: id=%s\n", request.request_id.c_str());
    }

  private:
    std::string       address_;
    std::atomic<bool> running_{ false };

    // Server-mode
    std::unique_ptr<LlamaServiceImpl>  service_;
    std::unique_ptr<grpc::Server>      server_;

    // Client-mode
    std::unique_ptr<llama_grpc::LlamaService::Stub> stub_;
    GRPCTransport::ResponseCallback                  response_callback_;
    GRPCTransport::StatusCallback                    status_callback_;
    std::thread                                      client_thread_;
};

// ═══════════════════════════════════════════════════════════════════════
// GRPCTransport public implementation (delegates to pimpl)
// ═══════════════════════════════════════════════════════════════════════

GRPCTransport::GRPCTransport(const std::string & address) :
    address_(address),
    pimpl_(std::make_unique<GRPCTransportImpl>(address)) {}

GRPCTransport::~GRPCTransport() = default;

bool GRPCTransport::start_server(RequestCallback on_request) {
    request_callback_ = std::move(on_request);
    is_server_        = true;
    running_          = true;
    return pimpl_->start_server(request_callback_);
}

void GRPCTransport::stop_server() {
    running_ = false;
    pimpl_->stop();
}

void GRPCTransport::send_response(const llama_dds::ChatCompletionResponse & response) {
    pimpl_->send_response(response);
}

void GRPCTransport::publish_status(const llama_dds::ServerStatus & status) {
    pimpl_->publish_status(status);
}

bool GRPCTransport::start_client() {
    if (!response_callback_) {
        fprintf(stderr, "[gRPC Client] subscribe_responses() must be called before start_client()\n");
        return false;
    }
    is_server_ = false;
    running_   = true;
    return pimpl_->start_client(response_callback_, status_callback_);
}

void GRPCTransport::stop_client() {
    running_ = false;
    pimpl_->stop();
}

void GRPCTransport::send_request(const llama_dds::ChatCompletionRequest & request) {
    pimpl_->send_request(request);
}

void GRPCTransport::subscribe_responses(ResponseCallback on_response) {
    response_callback_ = std::move(on_response);
}

void GRPCTransport::subscribe_status(StatusCallback on_status) {
    status_callback_ = std::move(on_status);
}

}  // namespace llama_grpc
