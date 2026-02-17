/**
 * DDS Benchmark Client
 * Measures latency for multiple DDS requests
 */

#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <vector>
#include <random>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <cstring>

// Include CycloneDDS
#include "dds/dds.h"
#include "idl/LlamaDDS.h"

// Topics
static const char* TOPIC_REQUEST = "llama_chat_completion_request";
static const char* TOPIC_RESPONSE = "llama_chat_completion_response";

// Generate UUID
static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);

    char buf[37];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        dis(gen), dis(gen), dis(gen), dis(gen),
        dis(gen), dis(gen),
        dis2(gen), dis(gen),
        dis2(gen), dis(gen),
        dis(gen), dis(gen), dis(gen), dis(gen), dis(gen), dis(gen));
    return std::string(buf);
}

struct BenchmarkResult {
    double latency_ms;
    std::string content;
    bool success;
};

BenchmarkResult send_request(dds_entity_t writer, dds_entity_t reader, const std::string& prompt, int max_tokens) {
    BenchmarkResult result;
    result.success = false;

    auto start = std::chrono::high_resolution_clock::now();

    // Create request
    llama_ChatCompletionRequest req;
    req.request_id = dds_string_dup(generate_uuid().c_str());
    req.model = dds_string_dup("phi4-mini");
    req.temperature = 0.3f;
    req.max_tokens = max_tokens;
    req.stream = false;

    // Setup messages
    req.messages._maximum = 1;
    req.messages._length = 1;
    req.messages._buffer = (llama_ChatMessage*)malloc(sizeof(llama_ChatMessage));
    req.messages._buffer[0].role = dds_string_dup("user");
    req.messages._buffer[0].content = dds_string_dup(prompt.c_str());

    // Write request
    dds_return_t ret = dds_write(writer, &req);
    if (ret != DDS_RETCODE_OK) {
        std::cerr << "Write failed: " << dds_strretcode(-ret) << std::endl;
        return result;
    }

    // Wait for response with polling
    bool received = false;
    for (int i = 0; i < 120; i++) {  // 60 second timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        void* samples[1];
        dds_sample_info_t infos[1];
        samples[0] = nullptr;
        memset(&infos[0], 0, sizeof(infos[0]));

        ret = dds_take(reader, samples, infos, 1, 1);

        if (ret > 0 && infos[0].valid_data) {
            auto* resp = static_cast<llama_ChatCompletionResponse*>(samples[0]);

            auto end = std::chrono::high_resolution_clock::now();
            result.latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
            result.content = resp->content ? std::string(resp->content) : "";
            result.success = true;
            received = true;
            break;
        }
    }

    if (!received) {
        std::cerr << "Timeout waiting for response" << std::endl;
    }

    return result;
}

int main(int argc, char* argv[]) {
    int domain_id = 0;
    int num_requests = 10;
    std::string prompt = "What is 2+2?";
    int max_tokens = 30;

    if (argc > 1) num_requests = std::atoi(argv[1]);
    if (argc > 2) prompt = argv[2];
    if (argc > 3) max_tokens = std::atoi(argv[3]);

    std::cout << "=== DDS Benchmark ===" << std::endl;
    std::cout << "Domain: " << domain_id << std::endl;
    std::cout << "Requests: " << num_requests << std::endl;
    std::cout << "Prompt: " << prompt << std::endl;
    std::cout << "Max tokens: " << max_tokens << std::endl;

    // Create participant
    dds_entity_t participant = dds_create_participant(domain_id, NULL, NULL);
    if (participant < 0) {
        std::cerr << "Failed to create participant: " << dds_strretcode(-participant) << std::endl;
        return 1;
    }

    // Create topics
    dds_entity_t request_topic = dds_create_topic(
        participant, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, NULL, NULL);
    if (request_topic < 0) {
        std::cerr << "Failed to create request topic: " << dds_strretcode(-request_topic) << std::endl;
        return 1;
    }

    dds_entity_t response_topic = dds_create_topic(
        participant, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, NULL, NULL);
    if (response_topic < 0) {
        std::cerr << "Failed to create response topic: " << dds_strretcode(-response_topic) << std::endl;
        return 1;
    }

    // Create writer and reader
    dds_entity_t writer = dds_create_writer(participant, request_topic, NULL, NULL);
    if (writer < 0) {
        std::cerr << "Failed to create writer: " << dds_strretcode(-writer) << std::endl;
        return 1;
    }

    dds_entity_t reader = dds_create_reader(participant, response_topic, NULL, NULL);
    if (reader < 0) {
        std::cerr << "Failed to create reader: " << dds_strretcode(-reader) << std::endl;
        return 1;
    }

    // Small delay to let DDS initialize
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "\n--- Running Benchmark ---" << std::endl;

    std::vector<double> latencies;
    int success_count = 0;

    for (int i = 0; i < num_requests; i++) {
        auto result = send_request(writer, reader, prompt, max_tokens);

        if (result.success) {
            latencies.push_back(result.latency_ms);
            success_count++;
            std::cout << "Request " << (i+1) << "/" << num_requests
                      << ": " << result.latency_ms << " ms" << std::endl;
        } else {
            std::cout << "Request " << (i+1) << "/" << num_requests << ": FAILED" << std::endl;
        }

        // Small delay between requests
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Calculate statistics
    if (latencies.empty()) {
        std::cerr << "\nNo successful requests!" << std::endl;
        return 1;
    }

    double sum = 0, mean, stddev = 0;
    for (double t : latencies) sum += t;
    mean = sum / latencies.size();

    for (double t : latencies) {
        stddev += (t - mean) * (t - mean);
    }
    stddev = std::sqrt(stddev / latencies.size());

    // Percentiles
    std::sort(latencies.begin(), latencies.end());
    size_t idx50 = (size_t)(latencies.size() * 0.50);
    size_t idx95 = (size_t)(latencies.size() * 0.95);
    size_t idx99 = (size_t)(latencies.size() * 0.99);
    double p50 = latencies[idx50];
    double p95 = latencies[idx95];
    double p99 = latencies[idx99];

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Successful: " << success_count << "/" << num_requests << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Mean: " << mean << " ms" << std::endl;
    std::cout << "Std Dev: " << stddev << " ms" << std::endl;
    std::cout << "p50: " << p50 << " ms" << std::endl;
    std::cout << "p95: " << p95 << " ms" << std::endl;
    std::cout << "p99: " << p99 << " ms" << std::endl;
    std::cout << "Throughput: " << (1000.0 / mean) << " req/s" << std::endl;

    // JSON output for parsing
    std::cout << "\n=== JSON ===" << std::endl;
    std::cout << "{\"mean\":" << mean << ",\"stddev\":" << stddev
              << ",\"p50\":" << p50 << ",\"p95\":" << p95 << ",\"p99\":" << p99
              << ",\"throughput\":" << (1000.0 / mean)
              << ",\"success\":" << success_count << "}" << std::endl;

    // Cleanup
    dds_delete(participant);

    return 0;
}
