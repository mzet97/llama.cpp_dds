/**
 * DDS Persistent Benchmark - Complete Thesis Version
 * Tests simple, medium, complex prompts
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
#include "dds_idl_wrapper.h" // For cleanup helper

static const char* TOPIC_REQUEST = "llama_chat_completion_request";
static const char* TOPIC_RESPONSE = "llama_chat_completion_response";

// Generate UUID v4 compliant
static std::string generate_uuid() {
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
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    char buf[37];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}

struct PromptTest {
    const char* name;
    const char* prompt;
};

PromptTest PROMPTS[] = {
    {"simple", "What is 2+2?"},
    {"medium", "Explain machine learning in a few sentences."},
    {"complex", "Write a detailed technical explanation of how neural networks work, including backpropagation, gradient descent, and the role of activation functions."}
};

std::vector<double> run_test(dds_entity_t writer, dds_entity_t reader, const char* prompt, int num_tests) {
    std::vector<double> latencies;

    for (int i = 0; i < num_tests; i++) {
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
        req.messages._buffer[0].content = dds_string_dup(prompt);

        dds_write(writer, &req);
        
        // Cleanup memory immediately after write
        llama_dds::free_llama_request(req);

        // Wait for final response
        bool received = false;
        
        // Create WaitSet for efficient waiting
        dds_entity_t ws = dds_create_waitset(dds_get_participant(reader));
        dds_waitset_attach(ws, reader, DDS_DATA_AVAILABLE_STATUS);
        
        dds_attach_t ws_results[1];
        
        while (!received) {
            dds_return_t rc = dds_waitset_wait(ws, ws_results, 1, DDS_SECS(30));
            
            if (rc > 0) {
                void* samples[1];
                dds_sample_info_t infos[1];
                samples[0] = dds_alloc(sizeof(llama_ChatCompletionResponse)); 
                
                int n = dds_take(reader, samples, infos, 1, 1);
                if (n > 0 && infos[0].valid_data) {
                    auto* resp = static_cast<llama_ChatCompletionResponse*>(samples[0]);
                    
                    if (resp->is_final) {
                        auto end = std::chrono::high_resolution_clock::now();
                        double ms = std::chrono::duration<double, std::milli>(end - start).count();
                        latencies.push_back(ms);
                        received = true;
                    }
                    
                    // Helper to free generated IDL types correctly if needed, 
                    // but dds_free handles the sample itself.
                    // However, we should check if deep copy was made or not.
                    // dds_alloc/dds_free is sufficient for samples taken from reader.
                }
                
                if (samples[0]) dds_free(samples[0]);
            } else {
                // Timeout or error
                break;
            }
        }
        
        dds_delete(ws);

        if (!received) {
            latencies.push_back(-1); // Timeout
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return latencies;
}

#include <fstream>

// ... existing code ...

int main(int argc, char* argv[]) {
    int num_tests = 10;
    if (argc > 1) num_tests = atoi(argv[1]);

    // Optional CSV file output
    std::ofstream csv_file;
    if (argc > 2) {
        csv_file.open(argv[2]);
        // Header
        csv_file << "prompt_type,mean,std,p50,p95,p99" << std::endl;
    }

    // Initialize DDS
    dds_entity_t participant = dds_create_participant(0, NULL, NULL);
    if (participant < 0) {
        std::cerr << "Failed to create participant: " << participant << std::endl;
        return 1;
    }

    dds_entity_t request_topic = dds_create_topic(participant, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, NULL, NULL);
    dds_entity_t response_topic = dds_create_topic(participant, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, NULL, NULL);

    // Create QoS for matching with server
    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8); // Added HISTORY

    dds_entity_t writer = dds_create_writer(participant, request_topic, qos, NULL);
    dds_entity_t reader = dds_create_reader(participant, response_topic, qos, NULL);

    dds_delete_qos(qos);

    std::cout << "DDS initialized. Waiting for server..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Run tests
    for (int p = 0; p < 3; p++) {
        std::cout << "\n--- " << PROMPTS[p].name << " ---" << std::endl;
        std::cout << "Prompt: " << PROMPTS[p].prompt << std::endl;

        auto results = run_test(writer, reader, PROMPTS[p].prompt, num_tests);

        // Calculate stats
        std::vector<double> valid;
        for (double t : results) if (t > 0) valid.push_back(t);

        if (valid.empty()) {
            std::cout << "No successful requests!" << std::endl;
            continue;
        }

        double sum = 0;
        for (double t : valid) sum += t;
        double mean = sum / valid.size();

        double stddev = 0;
        for (double t : valid) stddev += (t - mean) * (t - mean);
        stddev = std::sqrt(stddev / valid.size());

        std::sort(valid.begin(), valid.end());
        double p50 = valid[valid.size() * 0.50];
        double p95 = valid[valid.size() * 0.95];
        double p99 = valid[valid.size() * 0.99];

        std::cout << "Mean: " << mean << " ms" << std::endl;
        std::cout << "Std: " << stddev << " ms" << std::endl;
        std::cout << "p50: " << p50 << " ms" << std::endl;
        std::cout << "p95: " << p95 << " ms" << std::endl;
        std::cout << "p99: " << p99 << " ms" << std::endl;

        // CSV output to stdout
        std::cout << "\nCSV: " << PROMPTS[p].name << "," << mean << "," << stddev << "," << p50 << "," << p95 << "," << p99 << std::endl;
        
        // CSV output to file
        if (csv_file.is_open()) {
            csv_file << PROMPTS[p].name << "," << mean << "," << stddev << "," << p50 << "," << p95 << "," << p99 << std::endl;
        }
    }

    if (csv_file.is_open()) {
        csv_file.close();
        std::cout << "Results saved to " << argv[2] << std::endl;
    }

    dds_delete(participant);
    return 0;
}
