/**
 * DDS Persistent Benchmark Client
 *
 * Features:
 * - Persistent connection (no reload overhead)
 * - Optimized QoS for low latency
 * - Shared memory support
 * - Ready for thesis experiments
 *
 * Usage:
 *   ./benchmark_persistent [num_requests] [prompt]
 *
 * Or run in persistent mode (interactive):
 *   ./benchmark_persistent -i
 *   Then type prompts, one per line
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
#include <string>
#include <sstream>
#include <fstream>

// Include CycloneDDS
#include "dds/dds.h"
#include "idl/LlamaDDS.h"

// Topics
static const char* TOPIC_REQUEST = "llama_chat_completion_request";
static const char* TOPIC_RESPONSE = "llama_chat_completion_response";

// QoS Settings for Low Latency
// Based on CycloneDDS best practices for real-time systems
dds_qos_t* create_optimized_qos() {
    dds_qos_t* qos = dds_create_qos();

    // Reliability: BEST_EFFORT for lower latency
    // (acceptable for inference - we don't need guaranteed delivery)
    dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);

    // Durability: VOLATILE - no persistence (faster)
    dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);

    // Transport priority: HIGH for requests
    dds_qset_transport_priority(qos, 100);

    // Latency budget: minimal (0)
    dds_qset_latency_budget(qos, 0);

    // History: KEEP_LAST_1 (minimal memory)
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 1);

    // Resource limits: minimal
    dds_qset_resource_limits(qos, 10, 10, 10);

    // Ownership: SHARED (multiple readers/writers allowed)
    dds_qset_ownership(qos, DDS_OWNERSHIP_SHARED);

    return qos;
}

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
    std::string error;
};

// Persistent client state
struct PersistentClient {
    dds_entity_t participant;
    dds_entity_t request_writer;
    dds_entity_t response_reader;
    dds_qos_t* qos;

    PersistentClient() : participant(0), request_writer(0), response_reader(0), qos(nullptr) {}
};

bool init_client(PersistentClient& client, int domain_id) {
    // Create QoS
    client.qos = create_optimized_qos();

    // Create participant with config
    // Note: Use CYCLONEDDS_URI environment variable for XML config
    client.participant = dds_create_participant(domain_id, client.qos, NULL);
    if (client.participant < 0) {
        std::cerr << "Failed to create participant: " << dds_strretcode(-client.participant) << std::endl;
        return false;
    }

    // Create topics
    dds_entity_t request_topic = dds_create_topic(
        client.participant, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, client.qos, NULL);
    if (request_topic < 0) {
        std::cerr << "Failed to create request topic: " << dds_strretcode(-request_topic) << std::endl;
        return false;
    }

    dds_entity_t response_topic = dds_create_topic(
        client.participant, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, client.qos, NULL);
    if (response_topic < 0) {
        std::cerr << "Failed to create response topic: " << dds_strretcode(-response_topic) << std::endl;
        return false;
    }

    // Create writer with optimized QoS
    client.request_writer = dds_create_writer(client.participant, request_topic, client.qos, NULL);
    if (client.request_writer < 0) {
        std::cerr << "Failed to create writer: " << dds_strretcode(-client.request_writer) << std::endl;
        return false;
    }

    // Create reader with optimized QoS
    client.response_reader = dds_create_reader(client.participant, response_topic, client.qos, NULL);
    if (client.response_reader < 0) {
        std::cerr << "Failed to create reader: " << dds_strretcode(-client.response_reader) << std::endl;
        return false;
    }

    // Small delay for DDS to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return true;
}

BenchmarkResult send_request(PersistentClient& client, const std::string& prompt, int max_tokens) {
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
    dds_return_t ret = dds_write(client.request_writer, &req);
    if (ret != DDS_RETCODE_OK) {
        result.error = "Write failed: " + std::string(dds_strretcode(-ret));
        return result;
    }

    // Wait for response
    bool received = false;
    for (int i = 0; i < 60; i++) {  // 30 second timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        void* samples[1];
        dds_sample_info_t infos[1];
        samples[0] = nullptr;
        memset(&infos[0], 0, sizeof(infos[0]));

        ret = dds_take(client.response_reader, samples, infos, 1, 1);

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
        result.error = "Timeout waiting for response";
    }

    return result;
}

void print_qos_info() {
    std::cout << "\n=== QoS Settings (Optimized for Low Latency) ===" << std::endl;
    std::cout << "Reliability: BEST_EFFORT (lower latency)" << std::endl;
    std::cout << "Durability: VOLATILE (no persistence)" << std::endl;
    std::cout << "Transport Priority: 100 (high)" << std::endl;
    std::cout << "Latency Budget: 0 (minimal)" << std::endl;
    std::cout << "History: KEEP_LAST_1" << std::endl;
    std::cout << "===========================================" << std::endl;
}

int run_benchmark(PersistentClient& client, const std::string& prompt, int num_requests) {
    std::cout << "\n=== Running Benchmark ===" << std::endl;
    std::cout << "Prompt: " << prompt << std::endl;
    std::cout << "Requests: " << num_requests << std::endl;

    std::vector<double> latencies;
    int success_count = 0;

    for (int i = 0; i < num_requests; i++) {
        auto result = send_request(client, prompt, 30);

        if (result.success) {
            latencies.push_back(result.latency_ms);
            success_count++;
            std::cout << "Request " << (i+1) << "/" << num_requests
                      << ": " << result.latency_ms << " ms" << std::endl;
        } else {
            std::cout << "Request " << (i+1) << "/" << num_requests
                      << ": FAILED - " << result.error << std::endl;
        }

        // Small delay between requests
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (latencies.empty()) {
        std::cerr << "\nNo successful requests!" << std::endl;
        return 1;
    }

    // Calculate statistics
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
    std::cout << "Successful: " << success_count << "/" << num_requests << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Mean: " << mean << " ms" << std::endl;
    std::cout << "Std Dev: " << stddev << " ms" << std::endl;
    std::cout << "p50: " << p50 << " ms" << std::endl;
    std::cout << "p95: " << p95 << " ms" << std::endl;
    std::cout << "p99: " << p99 << " ms" << std::endl;
    std::cout << "Throughput: " << (1000.0 / mean) << " req/s" << std::endl;

    return 0;
}

int run_interactive(PersistentClient& client) {
    std::cout << "\n=== Interactive Mode ===" << std::endl;
    std::cout << "Type a prompt and press Enter to send." << std::endl;
    std::cout << "Type 'quit' to exit." << std::endl;
    std::cout << "Type 'bench N' to run N benchmark requests." << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") {
            break;
        }

        if (line.substr(0, 5) == "bench") {
            int n = 10;
            std::istringstream iss(line.substr(5));
            iss >> n;
            run_benchmark(client, "What is 2+2?", n);
            continue;
        }

        if (line.empty()) continue;

        auto result = send_request(client, line, 30);
        if (result.success) {
            std::cout << "Response (" << result.latency_ms << " ms): " << result.content << std::endl;
        } else {
            std::cout << "Error: " << result.error << std::endl;
        }
    }

    return 0;
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -i           Interactive mode" << std::endl;
    std::cout << "  -n N         Number of requests (default: 10)" << std::endl;
    std::cout << "  -p PROMPT    Prompt to send (default: 'What is 2+2?')" << std::endl;
    std::cout << "  -d DOMAIN    DDS domain (default: 0)" << std::endl;
    std::cout << "  -h           Show this help" << std::endl;
}

int main(int argc, char* argv[]) {
    int domain_id = 0;
    int num_requests = 10;
    std::string prompt = "What is 2+2?";
    bool interactive = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-i") {
            interactive = true;
        } else if (arg == "-n" && i + 1 < argc) {
            num_requests = std::atoi(argv[++i]);
        } else if (arg == "-p" && i + 1 < argc) {
            prompt = argv[++i];
        } else if (arg == "-d" && i + 1 < argc) {
            domain_id = std::atoi(argv[++i]);
        } else if (arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    std::cout << "=== DDS Persistent Benchmark Client ===" << std::endl;
    std::cout << "Domain: " << domain_id << std::endl;

    // Initialize client
    PersistentClient client;
    if (!init_client(client, domain_id)) {
        return 1;
    }

    print_qos_info();

    if (interactive) {
        return run_interactive(client);
    } else {
        return run_benchmark(client, prompt, num_requests);
    }
}
