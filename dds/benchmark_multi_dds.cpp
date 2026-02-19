/**
 * B1: Multi-Client DDS Benchmark
 *
 * Each process is a single DDS client that sends N sequential requests and
 * writes per-request latencies to a CSV.  The orchestration script launches
 * multiple instances in parallel and aggregates afterwards.
 *
 * Usage: benchmark_multi_dds <num_runs> <csv_file> [model] [client_id]
 */

#include "dds/dds.h"
#include "dds_idl_wrapper.h"
#include "dds_utils.h"
#include "idl/LlamaDDS.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static const char * TOPIC_REQUEST  = "llama_chat_completion_request";
static const char * TOPIC_RESPONSE = "llama_chat_completion_response";
static const char * DEFAULT_MODEL  = "tinyllama";

struct PromptDef {
    const char * name;
    const char * prompt;
};

static PromptDef PROMPTS[] = {
    { "simple",  "What is 2+2?"                        },
    { "complex",
     "Write a detailed technical explanation of how neural networks work, including backpropagation, gradient "
      "descent, and the role of activation functions." },
};
static constexpr int NUM_PROMPTS = 2;

// ---------------------------------------------------------------------------
// send_one: identical logic to benchmark_final — send & wait with request_id
// ---------------------------------------------------------------------------
static double send_one(dds_entity_t   writer,
                       dds_entity_t   reader,
                       dds_entity_t   ws,
                       dds_attach_t * ws_results,
                       const char *   prompt,
                       const char *   model) {
    std::string req_id = llama_dds::generate_uuid();

    llama_ChatCompletionRequest req;
    memset(&req, 0, sizeof(req));
    req.request_id                  = dds_string_dup(req_id.c_str());
    req.model                       = dds_string_dup(model);
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

    while (true) {
        dds_return_t rc = dds_waitset_wait(ws, ws_results, 1, DDS_SECS(120));
        if (rc > 0) {
            void *            samples[1] = { nullptr };
            dds_sample_info_t infos[1];
            int               n = dds_take(reader, samples, infos, 1, 1);
            if (n > 0 && infos[0].valid_data) {
                auto * resp = static_cast<llama_ChatCompletionResponse *>(samples[0]);
                if (resp->request_id && req_id == resp->request_id && resp->is_final) {
                    double ms =
                        std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - start)
                            .count();
                    dds_return_loan(reader, samples, n);
                    return ms;
                }
            }
            if (n > 0) {
                dds_return_loan(reader, samples, n);
            }
        } else {
            break;
        }
    }
    return -1.0;
}

// ---------------------------------------------------------------------------
// Compute & output stats identical to benchmark_final
// ---------------------------------------------------------------------------
struct Stats {
    double mean, stddev, p50, p95, p99;
};

static Stats compute_stats(std::vector<double> & v) {
    Stats s{};
    if (v.empty()) {
        return s;
    }
    double sum = 0;
    for (double x : v) {
        sum += x;
    }
    s.mean    = sum / v.size();
    double ss = 0;
    for (double x : v) {
        ss += (x - s.mean) * (x - s.mean);
    }
    s.stddev = v.size() > 1 ? std::sqrt(ss / (v.size() - 1)) : 0.0;
    std::sort(v.begin(), v.end());
    auto at = [&](double pct) {
        size_t idx = static_cast<size_t>(v.size() * pct);
        return v[std::min(idx, v.size() - 1)];
    };
    s.p50 = at(0.50);
    s.p95 = at(0.95);
    s.p99 = at(0.99);
    return s;
}

int main(int argc, char * argv[]) {
    int          num_runs  = 20;
    const char * csv_path  = nullptr;
    const char * model     = DEFAULT_MODEL;
    int          client_id = 0;

    if (argc > 1) {
        num_runs = atoi(argv[1]);
    }
    if (argc > 2) {
        csv_path = argv[2];
    }
    if (argc > 3) {
        model = argv[3];
    }
    if (argc > 4) {
        client_id = atoi(argv[4]);
    }

    // --- DDS init ---
    dds_entity_t participant = dds_create_participant(0, nullptr, nullptr);
    if (participant < 0) {
        std::cerr << "[C" << client_id << "] participant fail\n";
        return 1;
    }

    dds_entity_t req_topic =
        dds_create_topic(participant, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, nullptr, nullptr);
    dds_entity_t res_topic =
        dds_create_topic(participant, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, nullptr, nullptr);
    if (req_topic < 0 || res_topic < 0) {
        std::cerr << "[C" << client_id << "] topic fail\n";
        dds_delete(participant);
        return 1;
    }

    dds_qos_t * qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);

    dds_entity_t writer = dds_create_writer(participant, req_topic, qos, nullptr);
    dds_entity_t reader = dds_create_reader(participant, res_topic, qos, nullptr);
    dds_delete_qos(qos);
    if (writer < 0 || reader < 0) {
        std::cerr << "[C" << client_id << "] entity fail\n";
        dds_delete(participant);
        return 1;
    }

    // Active discovery (up to 10 s)
    {
        dds_instance_handle_t ih      = 0;
        int                   matched = 0;
        for (int i = 0; i < 100 && matched <= 0; ++i) {
            matched = (int) dds_get_matched_subscriptions(writer, &ih, 1);
            if (matched <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (matched <= 0) {
            std::cerr << "[C" << client_id << "] no server\n";
            dds_delete(participant);
            return 1;
        }
    }

    dds_entity_t ws = dds_create_waitset(participant);
    dds_waitset_attach(ws, reader, DDS_DATA_AVAILABLE_STATUS);
    dds_attach_t ws_results[1];

    // --- CSV ---
    std::ofstream csv;
    if (csv_path) {
        csv.open(csv_path);
        csv << "client_id,prompt_type,iteration,latency_ms\n";
    }

    auto wall_start = std::chrono::steady_clock::now();

    for (int p = 0; p < NUM_PROMPTS; ++p) {
        // warmup: 2 discarded
        for (int w = 0; w < 2; ++w) {
            send_one(writer, reader, ws, ws_results, PROMPTS[p].prompt, model);
        }

        // drain stale
        {
            void *            s[1] = { nullptr };
            dds_sample_info_t i[1];
            while (dds_take(reader, s, i, 1, 1) > 0) {
                dds_return_loan(reader, s, 1);
            }
        }

        for (int i = 0; i < num_runs; ++i) {
            double ms = send_one(writer, reader, ws, ws_results, PROMPTS[p].prompt, model);
            if (csv.is_open()) {
                csv << client_id << "," << PROMPTS[p].name << "," << i << "," << ms << "\n";
            }
        }
    }

    auto   wall_end = std::chrono::steady_clock::now();
    double wall_s   = std::chrono::duration<double>(wall_end - wall_start).count();

    // Summary line to stdout (for quick reading)
    std::cout << "[C" << client_id << "] done in " << wall_s << "s  (" << num_runs << " runs/prompt)\n";

    dds_delete(ws);
    dds_delete(participant);
    return 0;
}
