/**
 * Simple DDS client for testing llama.cpp DDS transport
 * Sends chat completion requests via DDS and receives responses
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

// Include CycloneDDS
#include "dds/dds.h"
#include "dds_idl_wrapper.h"  // For cleanup helper
#include "dds_utils.h"        // shared thread-safe UUID generator
#include "idl/LlamaDDS.h"

// Topics
static const char * TOPIC_REQUEST  = "llama_chat_completion_request";
static const char * TOPIC_RESPONSE = "llama_chat_completion_response";

int main(int argc, char * argv[]) {
    int domain_id = 0;
    if (argc > 1) {
        domain_id = std::atoi(argv[1]);
    }

    std::string prompt = "What is 2+2?";  // default
    if (argc > 2) {
        prompt = argv[2];
    }

    std::cout << "=== DDS Client Test ===" << std::endl;
    std::cout << "Connecting to domain " << domain_id << std::endl;
    std::cout << "Prompt: " << prompt << std::endl;

    // Create participant
    dds_entity_t participant = dds_create_participant(domain_id, NULL, NULL);
    if (participant < 0) {
        std::cerr << "Failed to create participant: " << dds_strretcode(-participant) << std::endl;
        return 1;
    }

    // Create topics
    dds_entity_t request_topic =
        dds_create_topic(participant, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, NULL, NULL);
    if (request_topic < 0) {
        std::cerr << "Failed to create request topic: " << dds_strretcode(-request_topic) << std::endl;
        return 1;
    }

    dds_entity_t response_topic =
        dds_create_topic(participant, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, NULL, NULL);
    if (response_topic < 0) {
        std::cerr << "Failed to create response topic: " << dds_strretcode(-response_topic) << std::endl;
        return 1;
    }

    // Create QoS for matching with server
    dds_qos_t * qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);  // Added HISTORY

    // Create writer for requests
    dds_entity_t request_writer = dds_create_writer(participant, request_topic, qos, NULL);
    if (request_writer < 0) {
        std::cerr << "Failed to create request writer: " << dds_strretcode(-request_writer) << std::endl;
        return 1;
    }

    // Create reader for responses
    dds_entity_t response_reader = dds_create_reader(participant, response_topic, qos, NULL);
    if (response_reader < 0) {
        std::cerr << "Failed to create response reader: " << dds_strretcode(-response_reader) << std::endl;
        return 1;
    }

    dds_delete_qos(qos);

    std::cout << "Topics created successfully" << std::endl;
    std::cout << "Request topic: " << TOPIC_REQUEST << std::endl;
    std::cout << "Response topic: " << TOPIC_RESPONSE << std::endl;

    // Wait for server to be ready
    std::cout << "Waiting for server..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Create and send a request
    llama_ChatCompletionRequest req;
    memset(&req, 0, sizeof(req));

    req.request_id  = dds_string_dup(llama_dds::generate_uuid().c_str());
    req.model       = dds_string_dup("phi4-mini");
    req.temperature = 0.3f;
    req.max_tokens  = 50;
    req.stream      = false;

    // Add test messages
    req.messages._maximum           = 1;
    req.messages._length            = 1;
    req.messages._buffer            = (llama_ChatMessage *) malloc(sizeof(llama_ChatMessage));
    req.messages._buffer[0].role    = dds_string_dup("user");
    req.messages._buffer[0].content = dds_string_dup(prompt.c_str());

    std::cout << "Sending request: " << req.request_id << std::endl;
    std::cout << "Model: " << req.model << std::endl;
    std::cout << "Temperature: " << req.temperature << std::endl;
    std::cout << "Max tokens: " << req.max_tokens << std::endl;

    dds_return_t ret = dds_write(request_writer, &req);

    // Cleanup memory immediately after write
    llama_dds::free_llama_request(req);

    if (ret != DDS_RETCODE_OK) {
        std::cerr << "Failed to write request: " << dds_strretcode(-ret) << std::endl;
    } else {
        std::cout << "Request sent successfully!" << std::endl;
    }

    // Wait for response
    std::cout << "Waiting for response..." << std::endl;

    // Use WaitSet
    dds_entity_t ws = dds_create_waitset(participant);
    dds_waitset_attach(ws, response_reader, DDS_DATA_AVAILABLE_STATUS);

    // Wait up to 30 seconds
    dds_attach_t ws_results[1];
    dds_return_t rc = dds_waitset_wait(ws, ws_results, 1, DDS_SECS(30));

    if (rc > 0) {
        // Use loan-based read: DDS manages internal string memory; return loan when done.
        void *            samples[1] = { nullptr };
        dds_sample_info_t infos[1];
        int               n = dds_take(response_reader, samples, infos, 1, 1);
        if (n > 0 && infos[0].valid_data) {
            llama_ChatCompletionResponse * resp = (llama_ChatCompletionResponse *) samples[0];
            std::cout << "\n=== Response received ===" << std::endl;
            std::cout << "Request ID: " << (resp->request_id ? resp->request_id : "(null)") << std::endl;
            std::cout << "Model: " << (resp->model ? resp->model : "(null)") << std::endl;
            std::cout << "Content: " << (resp->content ? resp->content : "(null)") << std::endl;
            std::cout << "Finish reason: " << (resp->finish_reason ? resp->finish_reason : "null") << std::endl;
            std::cout << "Is final: " << (resp->is_final ? "true" : "false") << std::endl;
            std::cout << "========================" << std::endl;
        } else {
            std::cerr << "Error reading response or invalid data" << std::endl;
        }
        // Return loan — DDS frees all internal strings correctly
        if (n > 0) {
            dds_return_loan(response_reader, samples, n);
        }
    } else {
        std::cout << "\nTimeout or no response received" << std::endl;
    }

    dds_delete(ws);

    std::cout << "\nTest complete!" << std::endl;

    // Cleanup
    dds_delete(participant);

    return 0;
}
