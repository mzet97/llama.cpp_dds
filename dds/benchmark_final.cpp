/**
 * DDS Persistent Benchmark - Complete Thesis Version
 * Tests simple, medium, complex prompts
 */

#include "dds/dds.h"
#include "dds_idl_wrapper.h"
#include "dds_utils.h"  // shared thread-safe UUID generator
#include "idl/LlamaDDS.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

static const char * TOPIC_REQUEST  = "llama_chat_completion_request";
static const char * TOPIC_RESPONSE = "llama_chat_completion_response";

// Default model name; can be overridden via argv[3].
static const char * DEFAULT_MODEL = "tinyllama";

struct PromptTest {
    const char * name;
    const char * prompt;
};

PromptTest PROMPTS[] = {
    { "simple",  "What is 2+2?"                                 },
    { "medium",  "Explain machine learning in a few sentences." },
    { "complex",
     "Write a detailed technical explanation of how neural networks work, including backpropagation, gradient "
      "descent, and the role of activation functions."          }
};

// Send one request and wait for the final response; return latency in ms (-1 on timeout).
static double send_one(dds_entity_t   writer,
                       dds_entity_t   reader,
                       dds_entity_t   ws,
                       dds_attach_t * ws_results,
                       const char *   prompt,
                       const char *   model_name) {
    std::string req_id = llama_dds::generate_uuid();

    llama_ChatCompletionRequest req;
    memset(&req, 0, sizeof(req));
    req.request_id                  = dds_string_dup(req_id.c_str());
    req.model                       = dds_string_dup(model_name);
    req.temperature                 = 0.3f;
    req.max_tokens                  = 30;
    req.stream                      = false;
    req.messages._maximum           = 1;
    req.messages._length            = 1;
    req.messages._buffer            = (llama_ChatMessage *) malloc(sizeof(llama_ChatMessage));
    req.messages._buffer[0].role    = dds_string_dup("user");
    req.messages._buffer[0].content = dds_string_dup(prompt);

    auto start = std::chrono::high_resolution_clock::now();
    dds_write(writer, &req);
    llama_dds::free_llama_request(req);

    bool received = false;
    while (!received) {
        dds_return_t rc = dds_waitset_wait(ws, ws_results, 1, DDS_SECS(60));
        if (rc > 0) {
            void *            samples[1] = { nullptr };
            dds_sample_info_t infos[1];
            int               n = dds_take(reader, samples, infos, 1, 1);
            if (n > 0 && infos[0].valid_data) {
                auto * resp     = static_cast<llama_ChatCompletionResponse *>(samples[0]);
                // Match by request_id to ignore stale responses from warmup or
                // previous iterations still queued in the DDS history buffer.
                bool   id_match = resp->request_id && (req_id == resp->request_id);
                if (id_match && resp->is_final) {
                    auto   end = std::chrono::high_resolution_clock::now();
                    double ms  = std::chrono::duration<double, std::milli>(end - start).count();
                    dds_return_loan(reader, samples, n);
                    return ms;
                }
            }
            if (n > 0) {
                dds_return_loan(reader, samples, n);
            }
        } else {
            break;  // timeout or error
        }
    }
    return -1.0;  // timeout
}

std::vector<double> run_test(dds_entity_t writer,
                             dds_entity_t reader,
                             const char * prompt,
                             int          num_tests,
                             const char * model_name) {
    std::vector<double> latencies;

    // Create WaitSet once and reuse across iterations to avoid per-call allocation overhead.
    dds_entity_t ws = dds_create_waitset(dds_get_participant(reader));
    dds_waitset_attach(ws, reader, DDS_DATA_AVAILABLE_STATUS);
    dds_attach_t ws_results[1];

    // Warmup: 2 discarded runs to prime KV-cache, page faults and thread pools.
    for (int w = 0; w < 2; w++) {
        send_one(writer, reader, ws, ws_results, prompt, model_name);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Drain any stale samples left in the DDS reader history after warmup
    // so measurement iterations don't waste cycles on non-matching request_ids.
    {
        void *            drain_samples[1] = { nullptr };
        dds_sample_info_t drain_infos[1];
        while (dds_take(reader, drain_samples, drain_infos, 1, 1) > 0) {
            dds_return_loan(reader, drain_samples, 1);
        }
    }

    for (int i = 0; i < num_tests; i++) {
        double ms = send_one(writer, reader, ws, ws_results, prompt, model_name);
        latencies.push_back(ms);
        // No artificial inter-iteration delay — measure back-to-back latency.
    }

    dds_delete(ws);  // delete the WaitSet after the loop

    return latencies;
}

int main(int argc, char * argv[]) {
    int num_tests = 64;  // N=64 for statistical significance per Cohen (1988)
    if (argc > 1) {
        num_tests = atoi(argv[1]);
    }

    // Optional CSV file output: argv[2]
    // Optional model name:      argv[3]  (default: tinyllama)
    const char * model_name = (argc > 3) ? argv[3] : DEFAULT_MODEL;

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
    if (request_topic < 0) {
        std::cerr << "Failed to create request topic: " << dds_strretcode(-request_topic) << std::endl;
        dds_delete(participant);
        return 1;
    }
    dds_entity_t response_topic =
        dds_create_topic(participant, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, NULL, NULL);
    if (response_topic < 0) {
        std::cerr << "Failed to create response topic: " << dds_strretcode(-response_topic) << std::endl;
        dds_delete(participant);
        return 1;
    }

    // Create QoS for matching with server
    dds_qos_t * qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);

    dds_entity_t writer = dds_create_writer(participant, request_topic, qos, NULL);
    if (writer < 0) {
        std::cerr << "Failed to create writer: " << dds_strretcode(-writer) << std::endl;
        dds_delete_qos(qos);
        dds_delete(participant);
        return 1;
    }
    dds_entity_t reader = dds_create_reader(participant, response_topic, qos, NULL);
    if (reader < 0) {
        std::cerr << "Failed to create reader: " << dds_strretcode(-reader) << std::endl;
        dds_delete_qos(qos);
        dds_delete(participant);
        return 1;
    }

    dds_delete_qos(qos);

    std::cout << "DDS initialized. Waiting for server discovery..." << std::endl;
    std::cout << "Model: " << model_name << "  Runs per prompt: " << num_tests << std::endl;

    // Active discovery: wait until the writer has matched with at least one
    // remote reader (the server's request reader).  Replaces a static 2 s sleep
    // and guarantees the server is actually reachable before benchmarking.
    {
        dds_instance_handle_t ih      = 0;
        int                   matched = 0;
        for (int attempt = 0; attempt < 100; ++attempt) {  // up to 10 s
            matched = (int) dds_get_matched_subscriptions(writer, &ih, 1);
            if (matched > 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (matched <= 0) {
            std::cerr << "No server discovered within 10 s — aborting." << std::endl;
            dds_delete(participant);
            return 1;
        }
        std::cout << "Server discovered (" << matched << " subscription(s) matched)." << std::endl;
    }

    // Run tests
    for (int p = 0; p < 3; p++) {
        std::cout << "\n--- " << PROMPTS[p].name << " ---" << std::endl;
        std::cout << "Prompt: " << PROMPTS[p].prompt << std::endl;

        auto results = run_test(writer, reader, PROMPTS[p].prompt, num_tests, model_name);

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

        // Sample stddev (Bessel's correction, ÷(N-1)) — consistent with Python statistics.stdev().
        double stddev = 0;
        for (double t : valid) {
            stddev += (t - mean) * (t - mean);
        }
        stddev = (valid.size() > 1) ? std::sqrt(stddev / (valid.size() - 1)) : 0.0;

        std::sort(valid.begin(), valid.end());
        const auto at = [&](double pct) -> double {
            // Clamp to [0, n-1] so n < 100/pct sample counts are safe.
            size_t idx = static_cast<size_t>(valid.size() * pct);
            if (idx >= valid.size()) {
                idx = valid.size() - 1;
            }
            return valid[idx];
        };
        double p50 = at(0.50);
        double p95 = at(0.95);
        double p99 = at(0.99);

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
