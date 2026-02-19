/**
 * B2: Streaming DDS Benchmark — TTFT & Inter-Token Latency
 *
 * Sends requests with stream=true and measures:
 *   - TTFT  = time from dds_write(request) to first received chunk (is_final=false)
 *   - ITL   = inter-token latency between successive chunks
 *   - Total = time from dds_write(request) to final chunk (is_final=true)
 *
 * Usage: benchmark_stream_dds <num_runs> <csv_file> [model]
 */

#include "dds/dds.h"
#include "dds_idl_wrapper.h"
#include "dds_utils.h"
#include "idl/LlamaDDS.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

static const char * TOPIC_REQUEST  = "llama_chat_completion_request";
static const char * TOPIC_RESPONSE = "llama_chat_completion_response";
static const char * DEFAULT_MODEL  = "tinyllama";

using Clock = std::chrono::high_resolution_clock;
using Ms    = std::chrono::duration<double, std::milli>;

struct StreamResult {
    double              ttft_ms;   // time-to-first-token
    double              total_ms;  // end-to-end
    int                 num_chunks;
    std::vector<double> itl_ms;    // inter-token latencies (between successive chunks)
};

// ---------------------------------------------------------------------------
// send_stream: send one streaming request and collect timing
// ---------------------------------------------------------------------------
static StreamResult send_stream(dds_entity_t   writer,
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
    req.max_tokens                  = 100;  // more tokens for meaningful ITL measurement
    req.stream                      = true;
    req.messages._maximum           = 1;
    req.messages._length            = 1;
    req.messages._buffer            = (llama_ChatMessage *) malloc(sizeof(llama_ChatMessage));
    req.messages._buffer[0].role    = dds_string_dup("user");
    req.messages._buffer[0].content = dds_string_dup(prompt);

    StreamResult result{};
    result.ttft_ms  = -1;
    result.total_ms = -1;

    auto t_start = Clock::now();
    dds_write(writer, &req);
    llama_dds::free_llama_request(req);

    auto t_prev      = t_start;
    bool first_chunk = true;

    while (true) {
        dds_return_t rc = dds_waitset_wait(ws, ws_results, 1, DDS_SECS(120));
        if (rc <= 0) {
            break;  // timeout
        }

        void *            samples[1] = { nullptr };
        dds_sample_info_t infos[1];
        int               n = dds_take(reader, samples, infos, 1, 1);
        if (n > 0 && infos[0].valid_data) {
            auto * resp = static_cast<llama_ChatCompletionResponse *>(samples[0]);
            if (!resp->request_id || req_id != resp->request_id) {
                dds_return_loan(reader, samples, n);
                continue;  // not our request
            }

            auto t_now = Clock::now();

            if (resp->is_final) {
                result.total_ms = Ms(t_now - t_start).count();
                if (first_chunk) {
                    // Edge case: final came without any partial chunks
                    result.ttft_ms = result.total_ms;
                }
                result.num_chunks++;
                dds_return_loan(reader, samples, n);
                break;
            }

            // Partial chunk (is_final == false)
            result.num_chunks++;
            if (first_chunk) {
                result.ttft_ms = Ms(t_now - t_start).count();
                first_chunk    = false;
            } else {
                result.itl_ms.push_back(Ms(t_now - t_prev).count());
            }
            t_prev = t_now;
            dds_return_loan(reader, samples, n);
        } else {
            if (n > 0) {
                dds_return_loan(reader, samples, n);
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Stats helpers
// ---------------------------------------------------------------------------
static double vec_mean(const std::vector<double> & v) {
    if (v.empty()) {
        return 0.0;
    }
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

static double vec_percentile(std::vector<double> v, double pct) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(v.size() * pct);
    return v[std::min(idx, v.size() - 1)];
}

static double vec_stddev(const std::vector<double> & v) {
    if (v.size() <= 1) {
        return 0.0;
    }
    double m  = vec_mean(v);
    double ss = 0;
    for (double x : v) {
        ss += (x - m) * (x - m);
    }
    return std::sqrt(ss / (v.size() - 1));  // Bessel's correction
}

int main(int argc, char * argv[]) {
    int          num_runs = 20;
    const char * csv_path = nullptr;
    const char * model    = DEFAULT_MODEL;

    if (argc > 1) {
        num_runs = atoi(argv[1]);
    }
    if (argc > 2) {
        csv_path = argv[2];
    }
    if (argc > 3) {
        model = argv[3];
    }

    // --- DDS init ---
    dds_entity_t participant = dds_create_participant(0, nullptr, nullptr);
    if (participant < 0) {
        std::cerr << "DDS participant fail\n";
        return 1;
    }

    dds_entity_t req_topic =
        dds_create_topic(participant, &llama_ChatCompletionRequest_desc, TOPIC_REQUEST, nullptr, nullptr);
    dds_entity_t res_topic =
        dds_create_topic(participant, &llama_ChatCompletionResponse_desc, TOPIC_RESPONSE, nullptr, nullptr);

    dds_qos_t * qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 32);  // larger buffer for streaming

    dds_entity_t writer = dds_create_writer(participant, req_topic, qos, nullptr);
    dds_entity_t reader = dds_create_reader(participant, res_topic, qos, nullptr);
    dds_delete_qos(qos);

    if (writer < 0 || reader < 0) {
        std::cerr << "DDS entity fail\n";
        dds_delete(participant);
        return 1;
    }

    // Active discovery
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
            std::cerr << "DDS: no server found\n";
            dds_delete(participant);
            return 1;
        }
        std::cout << "DDS: matched server.\n";
    }

    dds_entity_t ws = dds_create_waitset(participant);
    dds_waitset_attach(ws, reader, DDS_DATA_AVAILABLE_STATUS);
    dds_attach_t ws_results[1];

    // CSV
    std::ofstream csv;
    if (csv_path) {
        csv.open(csv_path);
        csv << "prompt_type,iteration,ttft_ms,itl_mean_ms,itl_p50_ms,itl_p95_ms,total_ms,num_chunks\n";
    }

    struct PromptDef {
        const char * name;
        const char * prompt;
    };

    PromptDef prompts[] = {
        { "complex",
         "Write a detailed technical explanation of how neural networks work, including backpropagation, gradient "
          "descent, and the role of activation functions." },
        { "simple",  "What is 2+2?"                        },
    };

    for (auto & pd : prompts) {
        std::cout << "\n--- Streaming: " << pd.name << " ---\n";

        // warmup
        for (int w = 0; w < 2; ++w) {
            send_stream(writer, reader, ws, ws_results, pd.prompt, model);
        }

        // drain stale
        {
            void *            s[1] = { nullptr };
            dds_sample_info_t i[1];
            while (dds_take(reader, s, i, 1, 1) > 0) {
                dds_return_loan(reader, s, 1);
            }
        }

        std::vector<double> ttfts, totals;
        std::vector<double> all_itl;

        for (int i = 0; i < num_runs; ++i) {
            auto r = send_stream(writer, reader, ws, ws_results, pd.prompt, model);
            ttfts.push_back(r.ttft_ms);
            totals.push_back(r.total_ms);
            all_itl.insert(all_itl.end(), r.itl_ms.begin(), r.itl_ms.end());

            if (csv.is_open()) {
                csv << pd.name << "," << i << "," << r.ttft_ms << "," << vec_mean(r.itl_ms) << ","
                    << vec_percentile(r.itl_ms, 0.50) << "," << vec_percentile(r.itl_ms, 0.95) << "," << r.total_ms
                    << "," << r.num_chunks << "\n";
            }
        }

        // Summary
        std::cout << "  TTFT  mean=" << vec_mean(ttfts) << " p50=" << vec_percentile(ttfts, 0.50)
                  << " p95=" << vec_percentile(ttfts, 0.95) << " std=" << vec_stddev(ttfts) << " ms\n";
        std::cout << "  ITL   mean=" << vec_mean(all_itl) << " p50=" << vec_percentile(all_itl, 0.50)
                  << " p95=" << vec_percentile(all_itl, 0.95) << " std=" << vec_stddev(all_itl) << " ms\n";
        std::cout << "  Total mean=" << vec_mean(totals) << " p50=" << vec_percentile(totals, 0.50)
                  << " p95=" << vec_percentile(totals, 0.95) << " std=" << vec_stddev(totals) << " ms\n";
    }

    dds_delete(ws);
    dds_delete(participant);
    return 0;
}
