#include "dds_transport.h"

#include "dds_idl_wrapper.h"
#include "dds_utils.h"  // shared, thread-safe UUID generator

#include <dds/dds.h>
#include <dds/ddsc/dds_public_error.h>
#include <dds/ddsc/dds_public_impl.h>

#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

// Topic names
static const char * TOPIC_REQUEST  = "llama_chat_completion_request";
static const char * TOPIC_RESPONSE = "llama_chat_completion_response";
static const char * TOPIC_STATUS   = "llama_server_status";

// generate_request_id wraps the shared UUID generator from dds_utils.h.
static std::string generate_request_id() {
    return llama_dds::generate_uuid();
}

namespace llama_dds {

class DDSTransportImpl {
  public:
    DDSTransportImpl(int domain_id) : domain_id_(domain_id) {
        fprintf(stderr, "[DDS] Transport created with domain ID: %d\n", domain_id);
    }

    ~DDSTransportImpl() { stop(); }

    bool start_server(llama_dds::DDSTransport::RequestCallback on_request) {
        try {
            // Create participant
            participant_ = dds_create_participant(domain_id_, nullptr, nullptr);
            if (participant_ < 0) {
                fprintf(stderr, "[DDS] Failed to create participant: %d\n", participant_);
                return false;
            }

            // Create topics - using type names that match registered types
            request_topic_ =
                dds_create_topic(participant_, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, nullptr, nullptr);
            if (request_topic_ < 0) {
                fprintf(stderr, "[DDS] Failed to create request topic: %d\n", request_topic_);
                return false;
            }

            response_topic_ =
                dds_create_topic(participant_, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, nullptr, nullptr);
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
            dds_qos_t * qos = dds_create_qos();
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

            // NOTE: Status is published periodically — BEST_EFFORT + VOLATILE avoids
            // accumulating stale history in the DDS broker for every heartbeat.
            dds_qos_t * status_qos = dds_create_qos();
            dds_qset_reliability(status_qos, DDS_RELIABILITY_BEST_EFFORT, 0);
            dds_qset_durability(status_qos, DDS_DURABILITY_VOLATILE);
            dds_qset_history(status_qos, DDS_HISTORY_KEEP_LAST, 1);

            // Create writer for status
            status_writer_ = dds_create_writer(participant_, status_topic_, status_qos, nullptr);
            dds_delete_qos(status_qos);
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
            fprintf(stderr, "[DDS] Topics: request='%s', response='%s', status='%s'\n", TOPIC_REQUEST, TOPIC_RESPONSE,
                    TOPIC_STATUS);

            return true;
        } catch (const std::exception & e) {
            fprintf(stderr, "[DDS] Exception: %s\n", e.what());
            stop();  // Cleanup on exception
            return false;
        }
    }

    void stop() {
        running_ = false;

        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }

        if (response_reader_thread_.joinable()) {
            response_reader_thread_.join();
        }

        for (dds_entity_t * e :
             { &response_writer_, &status_writer_, &request_reader_, &request_writer_, &response_reader_,
               &status_reader_, &request_topic_, &response_topic_, &status_topic_, &participant_ }) {
            if (*e > 0) {
                dds_delete(*e);
                *e = 0;
            }
        }

        fprintf(stderr, "[DDS] Transport stopped\n");
    }

    void send_response(const llama_dds::ChatCompletionResponse & response) {
        if (response_writer_ <= 0) {
            return;
        }

        auto         data = to_llama_response(response);
        dds_return_t ret  = dds_write(response_writer_, &data);
        free_llama_response(data);  // free strdup'd strings after dds_write
        if (ret != DDS_RETCODE_OK) {
            fprintf(stderr, "[DDS] Error sending response: %d\n", ret);
        }

        fprintf(stderr, "[DDS] Sent response for request: %s\n", response.request_id.c_str());
    }

    void publish_status(const llama_dds::ServerStatus & status) {
        if (status_writer_ <= 0) {
            return;
        }

        auto data = to_llama_status(status);
        dds_write(status_writer_, &data);
        free_llama_status(data);  // free strdup'd strings after dds_write
    }

    //
    // Client mode (A1)
    //
    bool start_client(llama_dds::DDSTransport::ResponseCallback on_response,
                      llama_dds::DDSTransport::StatusCallback   on_status) {
        try {
            participant_ = dds_create_participant(domain_id_, nullptr, nullptr);
            if (participant_ < 0) {
                fprintf(stderr, "[DDS Client] Failed to create participant: %d\n", participant_);
                return false;
            }

            request_topic_ =
                dds_create_topic(participant_, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, nullptr, nullptr);
            response_topic_ =
                dds_create_topic(participant_, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, nullptr, nullptr);
            status_topic_ = dds_create_topic(participant_, &llama_ServerStatus_desc, TOPIC_STATUS, nullptr, nullptr);

            if (request_topic_ < 0 || response_topic_ < 0 || status_topic_ < 0) {
                fprintf(stderr, "[DDS Client] Failed to create topics\n");
                return false;
            }

            // Reliable QoS matches server's Request/Response writers
            dds_qos_t * qos = dds_create_qos();
            dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
            dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
            dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);

            request_writer_  = dds_create_writer(participant_, request_topic_, qos, nullptr);
            response_reader_ = dds_create_reader(participant_, response_topic_, qos, nullptr);

            dds_delete_qos(qos);

            // Best-effort for status (matches server QoS on the status topic)
            dds_qos_t * status_qos = dds_create_qos();
            dds_qset_reliability(status_qos, DDS_RELIABILITY_BEST_EFFORT, 0);
            dds_qset_durability(status_qos, DDS_DURABILITY_VOLATILE);
            dds_qset_history(status_qos, DDS_HISTORY_KEEP_LAST, 1);
            status_reader_ = dds_create_reader(participant_, status_topic_, status_qos, nullptr);
            dds_delete_qos(status_qos);

            if (request_writer_ < 0 || response_reader_ < 0 || status_reader_ < 0) {
                fprintf(stderr, "[DDS Client] Failed to create reader/writer entities\n");
                return false;
            }

            response_callback_ = std::move(on_response);
            status_callback_   = std::move(on_status);
            running_.store(true, std::memory_order_release);

            // Thread reads responses (and optionally status)
            response_reader_thread_ = std::thread([this]() { client_response_loop(); });

            fprintf(stderr, "[DDS Client] started (domain %d)\n", domain_id_);
            return true;
        } catch (const std::exception & e) {
            fprintf(stderr, "[DDS Client] Exception: %s\n", e.what());
            stop();
            return false;
        }
    }

    void send_request(const llama_dds::ChatCompletionRequest & request) {
        if (request_writer_ <= 0) {
            fprintf(stderr, "[DDS Client] not started — call start_client() first\n");
            return;
        }
        auto         data = to_llama_request(request);
        dds_return_t ret  = dds_write(request_writer_, &data);
        free_llama_request(data);
        if (ret != DDS_RETCODE_OK) {
            fprintf(stderr, "[DDS Client] Failed to send request: %d\n", ret);
        } else {
            fprintf(stderr, "[DDS Client] Request sent: id=%s\n", request.request_id.c_str());
        }
    }

  private:
    void client_response_loop() {
        fprintf(stderr, "[DDS Client] Response reader loop started\n");

        dds_entity_t ws = dds_create_waitset(participant_);
        if (ws < 0) {
            return;
        }

        dds_waitset_attach(ws, response_reader_, DDS_DATA_AVAILABLE_STATUS);
        if (status_reader_ > 0) {
            dds_waitset_attach(ws, status_reader_, DDS_DATA_AVAILABLE_STATUS);
        }

        dds_attach_t ws_results[2];

        while (running_.load(std::memory_order_acquire)) {
            dds_return_t rc = dds_waitset_wait(ws, ws_results, 2, DDS_MSECS(500));
            if (rc < 0) {
                break;
            }
            if (rc == 0) {
                continue;
            }

            // Check response reader
            {
                void *            samples[1] = { nullptr };
                dds_sample_info_t infos[1];
                dds_return_t      n = dds_take(response_reader_, samples, infos, 1, 1);
                if (n > 0) {
                    if (infos[0].valid_data && response_callback_) {
                        auto * resp = static_cast<llama_ChatCompletionResponse *>(samples[0]);
                        try {
                            response_callback_(to_response(*resp));
                        } catch (const std::exception & e) {
                            fprintf(stderr, "[DDS Client] response callback error: %s\n", e.what());
                        }
                    }
                    dds_return_loan(response_reader_, samples, n);
                }
            }

            // Check status reader
            if (status_reader_ > 0 && status_callback_) {
                void *            samples[1] = { nullptr };
                dds_sample_info_t infos[1];
                dds_return_t      n = dds_take(status_reader_, samples, infos, 1, 1);
                if (n > 0) {
                    if (infos[0].valid_data) {
                        auto * st = static_cast<llama_ServerStatus *>(samples[0]);
                        try {
                            status_callback_(to_status(*st));
                        } catch (const std::exception & e) {
                            fprintf(stderr, "[DDS Client] status callback error: %s\n", e.what());
                        }
                    }
                    dds_return_loan(status_reader_, samples, n);
                }
            }
        }

        dds_delete(ws);
        fprintf(stderr, "[DDS Client] Response reader loop ended\n");
    }

    // (read_loop for server mode is below):
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
                void *            samples[1];
                dds_sample_info_t infos[1];

                // Use loan instead of copy/alloc for zero-copy read
                samples[0] = nullptr;

                dds_return_t n = dds_take(request_reader_, samples, infos, 1, 1);

                if (n > 0) {
                    if (infos[0].valid_data) {
                        auto * req = static_cast<llama_ChatCompletionRequest *>(samples[0]);
                        try {
                            // Copy data to our C++ structure before returning the loan
                            // This is unavoidable unless we change the callback signature to take raw pointers
                            auto request = to_request(*req);

                            fprintf(stderr, "[DDS] Received request: id=%s, model=%s\n", request.request_id.c_str(),
                                    request.model.c_str());

                            if (request_callback_) {
                                request_callback_(request);
                            }
                        } catch (const std::exception & e) {
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

    int               domain_id_;
    std::atomic<bool> running_{ false };

    dds_entity_t participant_    = 0;
    dds_entity_t request_topic_  = 0;
    dds_entity_t response_topic_ = 0;
    dds_entity_t status_topic_   = 0;

    // Server-mode entities
    dds_entity_t request_reader_  = 0;
    dds_entity_t response_writer_ = 0;
    dds_entity_t status_writer_   = 0;

    // Client-mode entities (A1)
    dds_entity_t request_writer_  = 0;
    dds_entity_t response_reader_ = 0;
    dds_entity_t status_reader_   = 0;

    std::thread reader_thread_;           // server read loop
    std::thread response_reader_thread_;  // client response loop (A1)

    llama_dds::DDSTransport::RequestCallback  request_callback_;
    llama_dds::DDSTransport::ResponseCallback response_callback_;  // client mode
    llama_dds::DDSTransport::StatusCallback   status_callback_;    // client mode
};

// DDSTransport implementation
DDSTransport::DDSTransport(int domain_id) :
    domain_id_(domain_id),
    pimpl_(std::make_unique<DDSTransportImpl>(domain_id)) {}

DDSTransport::~DDSTransport() = default;

bool DDSTransport::start_server(llama_dds::DDSTransport::RequestCallback on_request) {
    request_callback_ = std::move(on_request);
    is_server_        = true;
    running_          = true;
    return pimpl_->start_server(request_callback_);
}

void DDSTransport::stop_server() {
    running_ = false;
    pimpl_->stop();
}

void DDSTransport::send_response(const llama_dds::ChatCompletionResponse & response) {
    pimpl_->send_response(response);
}

void DDSTransport::publish_status(const llama_dds::ServerStatus & status) {
    pimpl_->publish_status(status);
}

bool DDSTransport::start_client() {
    is_server_ = false;
    running_   = true;
    return pimpl_->start_client(response_callback_, status_callback_);
}

void DDSTransport::stop_client() {
    running_ = false;
    pimpl_->stop();
}

void DDSTransport::send_request(const llama_dds::ChatCompletionRequest & request) {
    pimpl_->send_request(request);
}

void DDSTransport::subscribe_responses(llama_dds::DDSTransport::ResponseCallback on_response) {
    response_callback_ = std::move(on_response);
}

void DDSTransport::subscribe_status(llama_dds::DDSTransport::StatusCallback on_status) {
    status_callback_ = std::move(on_status);
}

}  // namespace llama_dds
