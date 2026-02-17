/**
 * DDS Persistent Client - Simplified version
 */

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <random>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "dds/dds.h"
#include "idl/LlamaDDS.h"

static const char* TOPIC_REQUEST = "llama_chat_completion_request";
static const char* TOPIC_RESPONSE = "llama_chat_completion_response";

static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);

    char buf[37];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        dis(gen), dis(gen), dis(gen), dis(gen),
        dis(gen), dis(gen), dis2(gen), dis(gen),
        dis2(gen), dis(gen), dis(gen), dis(gen),
        dis(gen), dis(gen), dis(gen), dis(gen));
    return std::string(buf);
}

int main(int argc, char* argv[]) {
    int num_requests = 5;
    if (argc > 1) num_requests = atoi(argv[1]);

    std::cout << "=== DDS Persistent Client ===" << std::endl;

    // Create participant
    dds_entity_t participant = dds_create_participant(0, NULL, NULL);
    if (participant < 0) {
        std::cerr << "Failed to create participant" << std::endl;
        return 1;
    }

    // Create topics
    dds_entity_t request_topic = dds_create_topic(
        participant, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, NULL, NULL);
    dds_entity_t response_topic = dds_create_topic(
        participant, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, NULL, NULL);

    // Create writer and reader
    dds_entity_t writer = dds_create_writer(participant, request_topic, NULL, NULL);
    dds_entity_t reader = dds_create_reader(participant, response_topic, NULL, NULL);

    std::cout << "Waiting for init..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "Running " << num_requests << " requests..." << std::endl;

    std::vector<double> latencies;

    for (int i = 0; i < num_requests; i++) {
        auto start = std::chrono::high_resolution_clock::now();

        // Create request
        llama_ChatCompletionRequest req;
        memset(&req, 0, sizeof(req));
        req.request_id = dds_string_dup(generate_uuid().c_str());
        req.model = dds_string_dup("phi4-mini");
        req.temperature = 0.3f;
        req.max_tokens = 30;
        req.stream = false;
        req.messages._maximum = 1;
        req.messages._length = 1;
        req.messages._buffer = (llama_ChatMessage*)malloc(sizeof(llama_ChatMessage));
        req.messages._buffer[0].role = dds_string_dup("user");
        req.messages._buffer[0].content = dds_string_dup("test");

        // Write
        dds_write(writer, &req);

        // Wait for response
        bool received = false;
        for (int w = 0; w < 30; w++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            void* samples[1];
            dds_sample_info_t infos[1];

            int n = dds_take(reader, samples, infos, 1, 1);
            if (n > 0 && infos[0].valid_data) {
                auto end = std::chrono::high_resolution_clock::now();
                double ms = std::chrono::duration<double, std::milli>(end - start).count();
                latencies.push_back(ms);
                std::cout << "Request " << (i+1) << ": " << ms << " ms" << std::endl;
                received = true;
                break;
            }
        }

        if (!received) {
            std::cout << "Request " << (i+1) << ": TIMEOUT" << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Print stats
    if (!latencies.empty()) {
        double sum = 0;
        for (double t : latencies) sum += t;
        double mean = sum / latencies.size();
        std::cout << "\nMean: " << mean << " ms" << std::endl;
    }

    dds_delete(participant);
    return 0;
}
