/**
 * DDS Persistent Benchmark - Complete Thesis Version
 * Tests simple, medium, complex prompts
 */

#include "dds/dds.h"
#include "dds_idl_wrapper.h"  // For cleanup helper
#include "dds_utils.h"        // M4/M5: shared thread-safe UUID generator
#include "idl/LlamaDDS.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

static const char * TOPIC_REQUEST  = "llama_chat_completion_request";
static const char * TOPIC_RESPONSE = "llama_chat_completion_response";

// M4: removed local generate_uuid() — use llama_dds::generate_uuid() from dds_utils.h

struct PromptTest {
    const char * name;
    const char * prompt;
};

PromptTest PROMPTS[] = {
    { "simple",  "What is 2+2?"                                 },
    { "medium",  "Explain machine learning in a few sentences." },
    { "complex",
     "Write a detailed technical explanation of how neural networks work, including backpropagation, gradient "
     "descent, and the role of activation functions."           }
};

std::vector<double> run_test(dds_entity_t writer, dds_entity_t reader, const char * prompt, int num_tests) {
    std::vector<double> latencies;

    // A6: Create WaitSet once, reuse across iterations
    dds_entity_t ws = dds_create_waitset(dds_get_participant(reader));
    dds_waitset_attach(ws, reader, DDS_DATA_AVAILABLE_STATUS);
    dds_attach_t ws_results[1];

    for (int i = 0; i < num_tests; i++) {
        auto start = std::chrono::high_resolution_clock::now();

        // Create request
        llama_ChatCompletionRequest req;
        memset(&req, 0, sizeof(req));
        req.request_id                  = dds_string_dup(llama_dds::generate_uuid().c_str());
        req.model                       = dds_string_dup("phi4-mini");
        req.temperature                 = 0.3f;
        req.max_tokens                  = 30;
        req.stream                      = false;
        req.messages._maximum           = 1;
        req.messages._length            = 1;
        req.messages._buffer            = (llama_ChatMessage *) malloc(sizeof(llama_ChatMessage));
        req.messages._buffer[0].role    = dds_string_dup("user");
        req.messages._buffer[0].content = dds_string_dup(prompt);

        dds_write(writer, &req);

        // Cleanup memory immediately after write
        llama_dds::free_llama_request(req);

        // Wait for final response
        bool received = false;

        while (!received) {
            dds_return_t rc = dds_waitset_wait(ws, ws_results, 1, DDS_SECS(30));

            if (rc > 0) {
                // A5: Use loan-based read — zero-copy, DDS manages memory
                void *            samples[1] = { nullptr };
                dds_sample_info_t infos[1];

                int n = dds_take(reader, samples, infos, 1, 1);
                if (n > 0 && infos[0].valid_data) {
                    auto * resp = static_cast<llama_ChatCompletionResponse *>(samples[0]);

                    if (resp->is_final) {
                        auto   end = std::chrono::high_resolution_clock::now();
                        double ms  = std::chrono::duration<double, std::milli>(end - start).count();
                        latencies.push_back(ms);
                        received = true;
                    }
                }

                // A5: Return loan — DDS frees all internal strings correctly
                if (n > 0) {
                    dds_return_loan(reader, samples, n);
                }
            } else {
                // Timeout or error
                break;
            }
        }

        if (!received) {
            latencies.push_back(-1);  // Timeout
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // A6: Delete WaitSet after loop
    dds_delete(ws);

    return latencies;
}

#include <fstream>

// ... existing code ...

int main(int argc, char * argv[]) {
    int num_tests = 10;
    if (argc > 1) {
        num_tests = atoi(argv[1]);
    }

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

    dds_entity_t request_topic =
        dds_create_topic(participant, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, NULL, NULL);
    dds_entity_t response_topic =
        dds_create_topic(participant, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, NULL, NULL);

    // Create QoS for matching with server
    dds_qos_t * qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);  // Added HISTORY

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
        for (double t : results) {
            if (t > 0) {
                valid.push_back(t);
            }
        }

        if (valid.empty()) {
            std::cout << "No successful requests!" << std::endl;
            continue;
        }

        double sum = 0;
        for (double t : valid) {
            sum += t;
        }
        double mean = sum / valid.size();

        double stddev = 0;
        for (double t : valid) {
            stddev += (t - mean) * (t - mean);
        }
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
        std::cout << "\nCSV: " << PROMPTS[p].name << "," << mean << "," << stddev << "," << p50 << "," << p95 << ","
                  << p99 << std::endl;

        // CSV output to file
        if (csv_file.is_open()) {
            csv_file << PROMPTS[p].name << "," << mean << "," << stddev << "," << p50 << "," << p95 << "," << p99
                     << std::endl;
        }
    }

    if (csv_file.is_open()) {
        csv_file.close();
        std::cout << "Results saved to " << argv[2] << std::endl;
    }

    dds_delete(participant);
    return 0;
}
