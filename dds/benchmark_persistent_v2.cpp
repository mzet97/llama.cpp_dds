/**
 * DDS Persistent Benchmark Client
 * Based on working test_client.cpp
 * Runs multiple requests without reinitializing DDS
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
    int num_requests = 10;
    if (argc > 1) num_requests = atoi(argv[1]);

    std::cout << "=== DDS Persistent Benchmark Client ===" << std::endl;
    std::cout << "Requests: " << num_requests << std::endl;

    // Create participant
    dds_entity_t participant = dds_create_participant(0, NULL, NULL);
    if (participant < 0) {
        std::cerr << "Failed to create participant: " << dds_strretcode(-participant) << std::endl;
        return 1;
    }

    // Create topics
    dds_entity_t request_topic = dds_create_topic(
        participant, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, NULL, NULL);
    if (request_topic < 0) {
        std::cerr << "Failed to create request topic" << std::endl;
        return 1;
    }

    dds_entity_t response_topic = dds_create_topic(
        participant, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, NULL, NULL);
    if (response_topic < 0) {
        std::cerr << "Failed to create response topic" << std::endl;
        return 1;
    }

    // Create writer
    dds_entity_t writer = dds_create_writer(participant, request_topic, NULL, NULL);
    if (writer < 0) {
        std::cerr << "Failed to create writer" << std::endl;
        return 1;
    }

    // Create reader
    dds_entity_t reader = dds_create_reader(participant, response_topic, NULL, NULL);
    if (reader < 0) {
        std::cerr << "Failed to create reader" << std::endl;
        return 1;
    }

    std::cout << "DDS initialized. Running benchmark..." << std::endl;

    // Wait for server
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::vector<double> latencies;

    // Run multiple requests
    for (int i = 0; i < num_requests; i++) {
        auto start = std::chrono::high_resolution_clock::now();

        // Create request - exactly like working test_client.cpp
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
        req.messages._buffer[0].content = dds_string_dup("What is 2+2?");

        // Write request
        dds_return_t ret = dds_write(writer, &req);
        if (ret != DDS_RETCODE_OK) {
            std::cerr << "Write failed: " << dds_strretcode(-ret) << std::endl;
            continue;
        }

        // Wait for response - exactly like working test_client.cpp
        bool received = false;
        for (int w = 0; w < 30; w++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            void* samples[1];
            dds_sample_info_t infos[1];
            samples[0] = nullptr;
            memset(&infos[0], 0, sizeof(infos[0]));

            ret = dds_take(reader, samples, infos, 1, 1);
            if (ret > 0 && infos[0].valid_data) {
                auto* resp = static_cast<llama_ChatCompletionResponse*>(samples[0]);

                auto end = std::chrono::high_resolution_clock::now();
                double ms = std::chrono::duration<double, std::milli>(end - start).count();
                latencies.push_back(ms);

                std::cout << "Request " << (i+1) << "/" << num_requests
                          << ": " << ms << " ms" << std::endl;
                received = true;
                break;
            }
        }

        if (!received) {
            std::cout << "Request " << (i+1) << "/" << num_requests << ": TIMEOUT" << std::endl;
        }

        // Small delay between requests
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Calculate statistics
    if (!latencies.empty()) {
        double sum = 0, mean, stddev = 0;
        for (double t : latencies) sum += t;
        mean = sum / latencies.size();

        for (double t : latencies) {
            stddev += (t - mean) * (t - mean);
        }
        stddev = std::sqrt(stddev / latencies.size());

        // Percentiles
        std::sort(latencies.begin(), latencies.end());
        double p50 = latencies[latencies.size() * 0.50];
        double p95 = latencies[latencies.size() * 0.95];
        double p99 = latencies[latencies.size() * 0.99];

        std::cout << "\n=== Results ===" << std::endl;
        std::cout << "Successful: " << latencies.size() << "/" << num_requests << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Mean: " << mean << " ms" << std::endl;
        std::cout << "Std Dev: " << stddev << " ms" << std::endl;
        std::cout << "p50: " << p50 << " ms" << std::endl;
        std::cout << "p95: " << p95 << " ms" << std::endl;
        std::cout << "p99: " << p99 << " ms" << std::endl;
        std::cout << "Throughput: " << (1000.0 / mean) << " req/s" << std::endl;
    }

    dds_delete(participant);
    return 0;
}
