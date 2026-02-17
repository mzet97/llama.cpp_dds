#include "dds_transport.h"

#include "dds_idl_wrapper.h"

#include <dds/dds.h>
#include <dds/ddsc/dds_public_impl.h>
#include <dds/ddsc/dds_public_error.h>

#include <chrono>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <mutex>

// Topic names
static const char* TOPIC_REQUEST = "llama_chat_completion_request";
static const char* TOPIC_RESPONSE = "llama_chat_completion_response";
static const char* TOPIC_STATUS = "llama_server_status";

// Generate unique request ID (UUID v4 compliant)
static std::string generate_request_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dis;

    uint8_t bytes[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t val = dis(gen);
        bytes[i] = (val >> 0) & 0xFF;
        bytes[i+1] = (val >> 8) & 0xFF;
        bytes[i+2] = (val >> 16) & 0xFF;
        bytes[i+3] = (val >> 24) & 0xFF;
    }

    // Set version (4) and variant (RFC 4122)
    bytes[6] = (bytes[6] & 0x0F) | 0x40;  // Version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80;  // Variant RFC 4122

    char buf[37];
    snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    
    return std::string(buf);
}

namespace llama_dds {

class DDSTransportImpl {
public:
    DDSTransportImpl(int domain_id) : domain_id_(domain_id) {
        fprintf(stderr, "[DDS] Transport created with domain ID: %d\n", domain_id);
    }

    ~DDSTransportImpl() {
        stop();
    }

    bool start_server(llama_dds::DDSTransport::RequestCallback on_request) {
        try {
            // Create participant
            participant_ = dds_create_participant(domain_id_, nullptr, nullptr);
            if (participant_ < 0) {
                fprintf(stderr, "[DDS] Failed to create participant: %d\n", participant_);
                return false;
            }

            // Create topics - using type names that match registered types
            request_topic_ = dds_create_topic(participant_, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, nullptr, nullptr);
            if (request_topic_ < 0) {
                fprintf(stderr, "[DDS] Failed to create request topic: %d\n", request_topic_);
                return false;
            }

            response_topic_ = dds_create_topic(participant_, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, nullptr, nullptr);
            if (response_topic_ < 0) {
                fprintf(stderr, "[DDS] Failed to create response topic: %d\n", response_topic_);
                return false;
            }

            status_topic_ = dds_create_topic(participant_, &llama_ServerStatus_desc, TOPIC_STATUS, nullptr, nullptr);
            if (status_topic_ < 0) {
                fprintf(stderr, "[DDS] Failed to create status topic: %d\n", status_topic_);
                return false;
            }

            // Create optimized QoS settings for low latency
            dds_qos_t* qos = dds_create_qos();
            dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
            dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
            dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);

            // Create reader for requests
            request_reader_ = dds_create_reader(participant_, request_topic_, qos, nullptr);
            if (request_reader_ < 0) {
                fprintf(stderr, "[DDS] Failed to create request reader: %d\n", request_reader_);
                return false;
            }

            // Create writer for responses
            response_writer_ = dds_create_writer(participant_, response_topic_, qos, nullptr);
            if (response_writer_ < 0) {
                fprintf(stderr, "[DDS] Failed to create response writer: %d\n", response_writer_);
                return false;
            }

            // Create writer for status
            status_writer_ = dds_create_writer(participant_, status_topic_, qos, nullptr);
            if (status_writer_ < 0) {
                fprintf(stderr, "[DDS] Failed to create status writer: %d\n", status_writer_);
                return false;
            }

            // Free QoS after use (entities keep their own copy)
            dds_delete_qos(qos);

            request_callback_ = std::move(on_request);
            running_.store(true, std::memory_order_release);

            // Start reader thread
            reader_thread_ = std::thread([this]() { read_loop(); });

            fprintf(stderr, "[DDS] Server started successfully\n");
            fprintf(stderr, "[DDS] Topics: request='%s', response='%s', status='%s'\n",
                    TOPIC_REQUEST, TOPIC_RESPONSE, TOPIC_STATUS);

            return true;
        } catch (const std::exception& e) {
            fprintf(stderr, "[DDS] Exception: %s\n", e.what());
            stop(); // Cleanup on exception
            return false;
        }
    }

    void stop() {
        running_ = false;

        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }

        if (response_writer_ > 0) {
            dds_delete(response_writer_);
            response_writer_ = 0;
        }

        if (status_writer_ > 0) {
            dds_delete(status_writer_);
            status_writer_ = 0;
        }

        if (request_reader_ > 0) {
            dds_delete(request_reader_);
            request_reader_ = 0;
        }

        if (request_topic_ > 0) {
            dds_delete(request_topic_);
            request_topic_ = 0;
        }

        if (response_topic_ > 0) {
            dds_delete(response_topic_);
            response_topic_ = 0;
        }

        if (status_topic_ > 0) {
            dds_delete(status_topic_);
            status_topic_ = 0;
        }

        if (participant_ > 0) {
            dds_delete(participant_);
            participant_ = 0;
        }

        fprintf(stderr, "[DDS] Transport stopped\n");
    }

    void send_response(const llama_dds::ChatCompletionResponse& response) {
        if (response_writer_ <= 0) return;

        auto data = to_llama_response(response);
        dds_return_t ret = dds_write(response_writer_, &data);
        if (ret != DDS_RETCODE_OK) {
            fprintf(stderr, "[DDS] Error sending response: %d\n", ret);
        }

        fprintf(stderr, "[DDS] Sent response for request: %s\n", response.request_id.c_str());
    }

    void publish_status(const llama_dds::ServerStatus& status) {
        if (status_writer_ <= 0) return;

        auto data = to_llama_status(status);
        dds_write(status_writer_, &data);
    }

private:
    void read_loop() {
        fprintf(stderr, "[DDS] Reader loop started\n");

        // Create waitset for event-driven processing
        dds_entity_t ws = dds_create_waitset(participant_);
        if (ws < 0) {
            fprintf(stderr, "[DDS] Failed to create waitset: %d\n", ws);
            return;
        }

        if (dds_waitset_attach(ws, request_reader_, DDS_DATA_AVAILABLE_STATUS) < 0) {
             fprintf(stderr, "[DDS] Failed to attach reader to waitset\n");
             dds_delete(ws);
             return;
        }

        dds_attach_t ws_results[1];

        while (running_.load(std::memory_order_acquire)) {
            // Wait for data with 500ms timeout to periodically check running_ flag
            dds_return_t rc = dds_waitset_wait(ws, ws_results, 1, DDS_MSECS(500));
            
            if (rc < 0) {
                fprintf(stderr, "[DDS] Waitset error: %d\n", rc);
                break;
            }
            
            if (rc > 0) {
                // Data available
                void* samples[1];
                dds_sample_info_t infos[1];
                
                // Use loan instead of copy/alloc for zero-copy read
                samples[0] = nullptr;
                
                dds_return_t n = dds_take(request_reader_, samples, infos, 1, 1);
                
                if (n > 0) {
                    if (infos[0].valid_data) {
                        auto* req = static_cast<llama_ChatCompletionRequest*>(samples[0]);
                        try {
                            // Copy data to our C++ structure before returning the loan
                            // This is unavoidable unless we change the callback signature to take raw pointers
                            auto request = to_request(*req);
                            
                            fprintf(stderr, "[DDS] Received request: id=%s, model=%s\n",
                                    request.request_id.c_str(), request.model.c_str());

                            if (request_callback_) {
                                request_callback_(request);
                            }
                        } catch (const std::exception& e) {
                            fprintf(stderr, "[DDS] Error processing request: %s\n", e.what());
                        }
                    }
                    
                    // Return the loan to DDS
                    dds_return_loan(request_reader_, samples, n);
                } else if (n < 0) {
                    fprintf(stderr, "[DDS] Error reading: %d\n", n);
                }
            }
        }

        dds_delete(ws);
        fprintf(stderr, "[DDS] Reader loop ended\n");
    }

    int domain_id_;
    std::atomic<bool> running_{false};

    dds_entity_t participant_ = 0;
    dds_entity_t request_topic_ = 0;
    dds_entity_t response_topic_ = 0;
    dds_entity_t status_topic_ = 0;
    dds_entity_t request_reader_ = 0;
    dds_entity_t response_writer_ = 0;
    dds_entity_t status_writer_ = 0;

    std::thread reader_thread_;
    llama_dds::DDSTransport::RequestCallback request_callback_;
};

// DDSTransport implementation
DDSTransport::DDSTransport(int domain_id)
    : domain_id_(domain_id), pimpl_(std::make_unique<DDSTransportImpl>(domain_id)) {}

DDSTransport::~DDSTransport() = default;

bool DDSTransport::start_server(llama_dds::DDSTransport::RequestCallback on_request) {
    request_callback_ = std::move(on_request);
    is_server_ = true;
    running_ = true;
    return pimpl_->start_server(request_callback_);
}

void DDSTransport::stop_server() {
    running_ = false;
    pimpl_->stop();
}

void DDSTransport::send_response(const llama_dds::ChatCompletionResponse& response) {
    pimpl_->send_response(response);
}

void DDSTransport::publish_status(const llama_dds::ServerStatus& status) {
    pimpl_->publish_status(status);
}

bool DDSTransport::start_client() {
    // Not implemented yet
    return false;
}

void DDSTransport::stop_client() {
    pimpl_->stop();
}

void DDSTransport::send_request(const llama_dds::ChatCompletionRequest& request) {
    // Client mode - would publish to DDS topic
}

void DDSTransport::subscribe_responses(llama_dds::DDSTransport::ResponseCallback on_response) {
    response_callback_ = std::move(on_response);
}

void DDSTransport::subscribe_status(llama_dds::DDSTransport::StatusCallback on_status) {
    status_callback_ = std::move(on_status);
}

} // namespace llama_dds
