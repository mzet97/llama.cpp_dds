#include "arg.h"
#include "common.h"
#include "llama.h"
#include "log.h"
#include "server-context.h"
#include "server-http.h"
#include "server-models.h"

#if defined(LLAMA_DDS)
#    include "dds/dds_bridge.h"
#    include "nlohmann/json.hpp"
#    include "nlohmann/json_fwd.hpp"
#    include "server-queue.h"
#    include "server-task.h"
#endif

#include <signal.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <thread>  // for std::thread::hardware_concurrency
#include <vector>

#if defined(_WIN32)
#    include <windows.h>
#endif

static std::function<void(int)> shutdown_handler;
static std::atomic_flag         is_terminating = ATOMIC_FLAG_INIT;

#if defined(LLAMA_DDS)
// Forward declaration of server context for accessing params
struct server_context;

// Convert DDS request to JSON for task creation
static json dds_request_to_json(const llama_dds::ChatCompletionRequest & dds_req, const std::string & model_name) {
    json data = json::object();

    // Model
    if (!dds_req.model.empty()) {
        data["model"] = dds_req.model;
    } else {
        data["model"] = model_name;
    }

    // Messages (for chat completion)
    if (!dds_req.messages.empty()) {
        json messages = json::array();
        for (const auto & msg : dds_req.messages) {
            messages.push_back({
                { "role",    msg.role    },
                { "content", msg.content }
            });
        }
        data["messages"] = messages;
    }

    // Sampling parameters
    if (dds_req.temperature > 0) {
        data["temperature"] = dds_req.temperature;
    }
    if (dds_req.top_p && *dds_req.top_p > 0 && *dds_req.top_p < 1.0) {
        data["top_p"] = *dds_req.top_p;
    }
    if (dds_req.max_tokens > 0) {
        data["max_tokens"] = dds_req.max_tokens;
        data["n_predict"]  = dds_req.max_tokens;
    }
    if (dds_req.stop && !dds_req.stop->empty()) {
        data["stop"] = *dds_req.stop;
    }

    // Stream
    data["stream"] = dds_req.stream;

    return data;
}

// Helper function to convert DDS request to server task and process it
static void process_dds_request(llama_dds::DDSBridge *                   dds_bridge,
                                const llama_dds::ChatCompletionRequest & dds_req,
                                struct server_queue *                    queue_tasks,
                                struct server_response *                 queue_results,
                                const struct llama_vocab *               vocab,
                                const std::string &                      model_name,
                                const server_context_meta *              meta,        // C4/C5: proper chat pipeline
                                const common_params *                    params_base  // C5: for params_from_json_cmpl
) {
    LOG_INF("[DDS] Processing request: %s\n", dds_req.request_id.c_str());

    // Convert DDS request to JSON (ordered_json = server's `json` type for oaicompat calls)
    json data = dds_request_to_json(dds_req, model_name);

    LOG_INF("[DDS] Request JSON: %s\n", data.dump(2).c_str());

    // C4: Apply the model's actual chat template via the server pipeline.
    // Falls back to a hardcoded Phi template only when meta is unavailable
    // (e.g., router mode before a model is loaded).
    std::string prompt;
    if (meta != nullptr) {
        // Use oaicompat_chat_params_parse which picks the correct template
        // registered for the loaded model (Phi, Llama-3, Mistral, etc.).
        std::vector<raw_buffer> files;
        // data is already `json` (nlohmann::ordered_json), so it binds directly to json& body
        json                    body_parsed = oaicompat_chat_params_parse(data, meta->chat_params, files);
        prompt                              = body_parsed.value("prompt", std::string{});
        // Propagate parsed body so params_from_json_cmpl sees all fields
        data                                = std::move(body_parsed);
    } else {
        // Fallback for router / model-less path
        for (const auto & msg : dds_req.messages) {
            if (msg.role == "system") {
                prompt += "<|system|>\n" + msg.content + "<|end|>\n";
            } else if (msg.role == "user") {
                prompt += "<|user|>\n" + msg.content + "<|end|>\n";
            } else if (msg.role == "assistant") {
                prompt += "<|assistant|>\n" + msg.content + "<|end|>\n";
            }
        }
        prompt += "<|assistant|>\n";
        // M3: ensure data["prompt"] is set so tokenize_input_prompts can consume it
        data["prompt"] = prompt;
    }

    LOG_INF("[DDS] Prompt: %s\n", prompt.substr(0, 100).c_str());

    // M3: Use tokenize_input_prompts — same pipeline as the HTTP endpoint.
    // mctx=nullptr → text-only tokenization (DDS does not carry multimodal data).
    // data["prompt"] is always populated: by oaicompat_chat_params_parse (meta != null)
    // or explicitly set in the fallback branch above.
    server_tokens tok_result;
    try {
        auto tok_vec = tokenize_input_prompts(vocab, nullptr, data.at("prompt"), true, true);
        if (tok_vec.empty()) {
            throw std::runtime_error("tokenize_input_prompts returned empty vector");
        }
        tok_result = std::move(tok_vec[0]);
    } catch (const std::exception & ex) {
        LOG_ERR("[DDS] Failed to tokenize prompt: %s\n", ex.what());
        llama_dds::ChatCompletionResponse resp;
        resp.request_id    = dds_req.request_id;
        resp.model         = dds_req.model.empty() ? model_name : dds_req.model;
        resp.content       = std::string("[DDS] Error: Failed to tokenize prompt: ") + ex.what();
        resp.is_final      = true;
        resp.finish_reason = "error";
        dds_bridge->send_response(resp);
        return;
    }
    LOG_INF("[DDS] Tokenized to %zu tokens\n", tok_result.size());

    // Create a task
    server_task task(SERVER_TASK_TYPE_COMPLETION);
    task.id = queue_tasks->get_new_id();

    // M3: tokens set via proper pipeline
    task.tokens = std::move(tok_result);

    // C5: Use proper server pipeline to configure ALL sampling params.
    // Falls back to minimal manual params if meta/params_base are unavailable.
    if (meta != nullptr && params_base != nullptr) {
        task.params = server_task::params_from_json_cmpl(vocab, *params_base, meta->slot_n_ctx, data);
    } else {
        task.params.n_predict     = dds_req.max_tokens > 0 ? dds_req.max_tokens : 50;
        task.params.sampling.temp = dds_req.temperature > 0 ? dds_req.temperature : 0.7f;
    }

    LOG_INF("[DDS] Posting task to queue, id=%d, tokens=%zu\n", task.id, task.tokens.size());

    // C6: Capture task_id before std::move(task) consumes it
    const int task_id = task.id;

    // Add task ID to waiting list before posting
    queue_results->add_waiting_task_id(task_id);

    // Post the task to the queue
    queue_tasks->post(std::move(task));

    // Wait for the result using the proper queue mechanism
    LOG_INF("[DDS] Waiting for result (stream=%d)...\n", (int) dds_req.stream);

    std::string generated_text;
    int         prompt_tokens     = 0;
    int         completion_tokens = 0;
    std::string finish_reason     = "stop";

    // Keep receiving results until we get a final one or timeout
    auto      start_wait   = std::chrono::steady_clock::now();
    // M8: compute once — timeout derived from --dds-timeout param (default 60s)
    const int timeout_secs = (params_base != nullptr) ? params_base->dds_timeout_secs : 60;
    bool      is_final     = false;
    int       result_count = 0;

    while (!is_final) {
        // Check timeout
        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_wait).count();
        if (elapsed > timeout_secs) {
            LOG_WRN("[DDS] Timeout (%ds) waiting for final result\n", timeout_secs);
            break;
        }

        // Wait for result with a short timeout
        auto result = queue_results->recv_with_timeout({ task_id }, 5);
        result_count++;

        if (result == nullptr) {
            // Timeout, continue waiting
            LOG_INF("[DDS] Timeout waiting (attempt %d)\n", result_count);
            continue;
        }

        LOG_INF("[DDS] Got result #%d\n", result_count);

        // Check if it's a final completion result
        auto * cmpl_final = dynamic_cast<server_task_result_cmpl_final *>(result.get());
        if (cmpl_final != nullptr) {
            prompt_tokens     = cmpl_final->n_prompt_tokens;
            completion_tokens = cmpl_final->n_decoded;

            // Determine finish reason
            switch (cmpl_final->stop) {
                case STOP_TYPE_EOS:
                    finish_reason = "stop";
                    break;
                case STOP_TYPE_LIMIT:
                    finish_reason = "length";
                    break;
                default:
                    finish_reason = "stop";
                    break;
            }

            if (dds_req.stream) {
                // A2: streaming — send the last chunk (may have remaining content)
                if (!cmpl_final->content.empty()) {
                    llama_dds::ChatCompletionResponse chunk;
                    chunk.request_id        = dds_req.request_id;
                    chunk.model             = dds_req.model.empty() ? model_name : dds_req.model;
                    chunk.content           = cmpl_final->content;
                    chunk.is_final          = false;
                    chunk.prompt_tokens     = prompt_tokens;
                    chunk.completion_tokens = completion_tokens;
                    dds_bridge->send_response(chunk);
                }
                generated_text = "";  // already streamed, nothing to send in final batch
            } else {
                generated_text = cmpl_final->content;
            }

            LOG_INF("[DDS] Got FINAL completion: %d prompt tokens, %d completion tokens\n", prompt_tokens,
                    completion_tokens);
            is_final = true;

        } else {
            // Check for partial completion result
            auto * cmpl_partial = dynamic_cast<server_task_result_cmpl_partial *>(result.get());
            if (cmpl_partial != nullptr) {
                prompt_tokens     = cmpl_partial->n_prompt_tokens;
                completion_tokens = cmpl_partial->n_decoded;

                if (dds_req.stream) {
                    // A2: streaming — publish each partial chunk immediately as a DDS sample
                    if (!cmpl_partial->content.empty()) {
                        llama_dds::ChatCompletionResponse chunk;
                        chunk.request_id        = dds_req.request_id;
                        chunk.model             = dds_req.model.empty() ? model_name : dds_req.model;
                        chunk.content           = cmpl_partial->content;
                        chunk.is_final          = false;
                        chunk.prompt_tokens     = prompt_tokens;
                        chunk.completion_tokens = completion_tokens;
                        dds_bridge->send_response(chunk);
                        LOG_INF("[DDS] Streamed chunk: %zu chars (n_decoded=%d)\n", cmpl_partial->content.size(),
                                completion_tokens);
                    }
                } else {
                    // Non-streaming: accumulate
                    generated_text += cmpl_partial->content;
                    LOG_INF("[DDS] Got partial: %zu chars total (n_decoded=%d)\n", generated_text.size(),
                            completion_tokens);

                    if (!cmpl_partial->is_progress && completion_tokens >= dds_req.max_tokens) {
                        finish_reason = "stop";
                        LOG_INF("[DDS] Received full completion (%d tokens)\n", completion_tokens);
                        is_final = true;
                    }
                }
            } else {
                // Check for error result
                auto * error_result = dynamic_cast<server_task_result_error *>(result.get());
                if (error_result != nullptr) {
                    generated_text = "[Error: " + error_result->err_msg + "]";
                    finish_reason  = "error";
                    LOG_ERR("[DDS] Task error: %s\n", error_result->err_msg.c_str());
                    is_final = true;
                }
            }
        }
    }

    LOG_INF("[DDS] Sending final response\n");

    // Send the terminal response — for streaming this carries is_final=true with empty content;
    // for non-streaming it carries the complete generated text.
    llama_dds::ChatCompletionResponse resp;
    resp.request_id        = dds_req.request_id;
    resp.model             = dds_req.model.empty() ? model_name : dds_req.model;
    resp.content           = generated_text;  // empty for streaming (chunks already sent)
    resp.is_final          = true;
    resp.finish_reason     = finish_reason;
    resp.prompt_tokens     = prompt_tokens;
    resp.completion_tokens = completion_tokens;

    dds_bridge->send_response(resp);

    // C6: Remove task_id from waiting list after completion/timeout
    queue_results->remove_waiting_task_id(task_id);

    LOG_INF("[DDS] Response sent for request: %s\n", dds_req.request_id.c_str());
}

// DDS polling thread function
static void dds_poll_loop(llama_dds::DDSBridge *      dds_bridge,
                          struct server_queue *       queue_tasks,
                          struct server_response *    queue_results,
                          const struct llama_vocab *  vocab,
                          std::atomic<bool> *         running,
                          const std::string &         model_name,
                          const server_context_meta * meta,        // Fase 2: typed, replaces void*
                          const common_params *       params_base  // Fase 2: for params_from_json_cmpl
) {
    LOG_INF("[DDS] Polling thread started\n");

    while (running->load()) {
        // A4: block until a request arrives or 100ms elapses — avoids 1000 wakeups/s idle
        dds_bridge->wait_for_request(std::chrono::milliseconds(100));

        llama_dds::ChatCompletionRequest req;
        // Atomic pop - returns false if empty
        if (dds_bridge->pop_pending_request(req)) {
            process_dds_request(dds_bridge, req, queue_tasks, queue_results, vocab, model_name, meta, params_base);
        }
    }

    LOG_INF("[DDS] Polling thread stopped\n");
}
#endif

static inline void signal_handler(int signal) {
    if (is_terminating.test_and_set()) {
        // in case it hangs, we can force terminate the server by hitting Ctrl+C twice
        // this is for better developer experience, we can remove when the server is stable enough
        fprintf(stderr, "Received second interrupt, terminating immediately.\n");
        exit(1);
    }

    shutdown_handler(signal);
}

// wrapper function that handles exceptions and logs errors
// this is to make sure handler_t never throws exceptions; instead, it returns an error response
static server_http_context::handler_t ex_wrapper(server_http_context::handler_t func) {
    return [func = std::move(func)](const server_http_req & req) -> server_http_res_ptr {
        std::string message;
        error_type  error;
        try {
            return func(req);
        } catch (const std::invalid_argument & e) {
            // treat invalid_argument as invalid request (400)
            error   = ERROR_TYPE_INVALID_REQUEST;
            message = e.what();
        } catch (const std::exception & e) {
            // treat other exceptions as server error (500)
            error   = ERROR_TYPE_SERVER;
            message = e.what();
        } catch (...) {
            error   = ERROR_TYPE_SERVER;
            message = "unknown error";
        }

        auto res    = std::make_unique<server_http_res>();
        res->status = 500;
        try {
            json error_data = format_error_response(message, error);
            res->status     = json_value(error_data, "code", 500);
            res->data       = safe_json_to_str({
                { "error", error_data }
            });
            SRV_WRN("got exception: %s\n", res->data.c_str());
        } catch (const std::exception & e) {
            SRV_ERR("got another exception: %s | while handling exception: %s\n", e.what(), message.c_str());
            res->data = "Internal Server Error";
        }
        return res;
    };
}

int main(int argc, char ** argv) {
    // own arguments required by this example
    common_params params;

    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_SERVER)) {
        return 1;
    }

    // validate batch size for embeddings
    // embeddings require all tokens to be processed in a single ubatch
    // see https://github.com/ggml-org/llama.cpp/issues/12836
    if (params.embedding && params.n_batch > params.n_ubatch) {
        LOG_WRN("%s: embeddings enabled with n_batch (%d) > n_ubatch (%d)\n", __func__, params.n_batch,
                params.n_ubatch);
        LOG_WRN("%s: setting n_batch = n_ubatch = %d to avoid assertion failure\n", __func__, params.n_ubatch);
        params.n_batch = params.n_ubatch;
    }

    if (params.n_parallel < 0) {
        LOG_INF("%s: n_parallel is set to auto, using n_parallel = 4 and kv_unified = true\n", __func__);

        params.n_parallel = 4;
        params.kv_unified = true;
    }

    // for consistency between server router mode and single-model mode, we set the same model name as alias
    if (params.model_alias.empty() && !params.model.name.empty()) {
        params.model_alias = params.model.name;
    }

    common_init();

    // struct that contains llama context and inference
    server_context ctx_server;

    llama_backend_init();
    llama_numa_init(params.numa);

    LOG_INF("system info: n_threads = %d, n_threads_batch = %d, total_threads = %d\n", params.cpuparams.n_threads,
            params.cpuparams_batch.n_threads, std::thread::hardware_concurrency());
    LOG_INF("\n");
    LOG_INF("%s\n", common_params_get_system_info(params).c_str());
    LOG_INF("\n");

    server_http_context ctx_http;
    if (!ctx_http.init(params)) {
        LOG_ERR("%s: failed to initialize HTTP server\n", __func__);
        return 1;
    }

#if defined(LLAMA_DDS)
    std::unique_ptr<llama_dds::DDSBridge> dds_bridge;
    std::thread                           dds_poll_thread;
    std::atomic<bool>                     dds_running{ false };
    // Fase 2: snapshot meta kept alive until dds_poll_thread.join() at shutdown
    std::unique_ptr<server_context_meta>  dds_meta;

    if (params.enable_dds) {
        LOG_INF("%s: initializing DDS transport on domain %d\n", __func__, params.dds_domain);
        dds_bridge = std::make_unique<llama_dds::DDSBridge>(params.dds_domain);
        if (!dds_bridge->init()) {
            LOG_ERR("%s: failed to initialize DDS bridge\n", __func__);
            return 1;
        }
        // M2: no set_process_callback needed — requests are queued via handle_request
        //     and processed by dds_poll_loop through pop_pending_request.
        if (!dds_bridge->start()) {
            LOG_ERR("%s: failed to start DDS bridge\n", __func__);
            return 1;
        }

        // Start DDS polling thread after server is ready
        dds_running = true;
        LOG_INF("%s: DDS transport enabled on domain %d\n", __func__, params.dds_domain);
    }
#endif

    //
    // Router
    //

    // register API routes
    server_routes routes(params, ctx_server);

    bool                                is_router_server = params.model.path.empty();
    std::optional<server_models_routes> models_routes{};
    if (is_router_server) {
        // setup server instances manager
        try {
            models_routes.emplace(params, argc, argv);
        } catch (const std::exception & e) {
            LOG_ERR("%s: failed to initialize router models: %s\n", __func__, e.what());
            return 1;
        }

        // proxy handlers
        // note: routes.get_health stays the same
        routes.get_metrics                 = models_routes->proxy_get;
        routes.post_props                  = models_routes->proxy_post;
        routes.get_api_show                = models_routes->proxy_get;
        routes.post_completions            = models_routes->proxy_post;
        routes.post_completions_oai        = models_routes->proxy_post;
        routes.post_chat_completions       = models_routes->proxy_post;
        routes.post_responses_oai          = models_routes->proxy_post;
        routes.post_anthropic_messages     = models_routes->proxy_post;
        routes.post_anthropic_count_tokens = models_routes->proxy_post;
        routes.post_infill                 = models_routes->proxy_post;
        routes.post_embeddings             = models_routes->proxy_post;
        routes.post_embeddings_oai         = models_routes->proxy_post;
        routes.post_rerank                 = models_routes->proxy_post;
        routes.post_tokenize               = models_routes->proxy_post;
        routes.post_detokenize             = models_routes->proxy_post;
        routes.post_apply_template         = models_routes->proxy_post;
        routes.get_lora_adapters           = models_routes->proxy_get;
        routes.post_lora_adapters          = models_routes->proxy_post;
        routes.get_slots                   = models_routes->proxy_get;
        routes.post_slots                  = models_routes->proxy_post;

        // custom routes for router
        routes.get_props  = models_routes->get_router_props;
        routes.get_models = models_routes->get_router_models;
        ctx_http.post("/models/load", ex_wrapper(models_routes->post_router_models_load));
        ctx_http.post("/models/unload", ex_wrapper(models_routes->post_router_models_unload));
    }

    ctx_http.get("/health", ex_wrapper(routes.get_health));     // public endpoint (no API key check)
    ctx_http.get("/v1/health", ex_wrapper(routes.get_health));  // public endpoint (no API key check)
    ctx_http.get("/metrics", ex_wrapper(routes.get_metrics));
    ctx_http.get("/props", ex_wrapper(routes.get_props));
    ctx_http.post("/props", ex_wrapper(routes.post_props));
    ctx_http.post("/api/show", ex_wrapper(routes.get_api_show));
    ctx_http.get("/models", ex_wrapper(routes.get_models));     // public endpoint (no API key check)
    ctx_http.get("/v1/models", ex_wrapper(routes.get_models));  // public endpoint (no API key check)
    ctx_http.get("/api/tags",
                 ex_wrapper(routes.get_models));  // ollama specific endpoint. public endpoint (no API key check)
    ctx_http.post("/completion", ex_wrapper(routes.post_completions));  // legacy
    ctx_http.post("/completions", ex_wrapper(routes.post_completions));
    ctx_http.post("/v1/completions", ex_wrapper(routes.post_completions_oai));
    ctx_http.post("/chat/completions", ex_wrapper(routes.post_chat_completions));
    ctx_http.post("/v1/chat/completions", ex_wrapper(routes.post_chat_completions));
    ctx_http.post("/api/chat", ex_wrapper(routes.post_chat_completions));       // ollama specific endpoint
    ctx_http.post("/v1/responses", ex_wrapper(routes.post_responses_oai));
    ctx_http.post("/v1/messages", ex_wrapper(routes.post_anthropic_messages));  // anthropic messages API
    ctx_http.post("/v1/messages/count_tokens",
                  ex_wrapper(routes.post_anthropic_count_tokens));              // anthropic token counting
    ctx_http.post("/infill", ex_wrapper(routes.post_infill));
    ctx_http.post("/embedding", ex_wrapper(routes.post_embeddings));            // legacy
    ctx_http.post("/embeddings", ex_wrapper(routes.post_embeddings));
    ctx_http.post("/v1/embeddings", ex_wrapper(routes.post_embeddings_oai));
    ctx_http.post("/rerank", ex_wrapper(routes.post_rerank));
    ctx_http.post("/reranking", ex_wrapper(routes.post_rerank));
    ctx_http.post("/v1/rerank", ex_wrapper(routes.post_rerank));
    ctx_http.post("/v1/reranking", ex_wrapper(routes.post_rerank));
    ctx_http.post("/tokenize", ex_wrapper(routes.post_tokenize));
    ctx_http.post("/detokenize", ex_wrapper(routes.post_detokenize));
    ctx_http.post("/apply-template", ex_wrapper(routes.post_apply_template));
    // LoRA adapters hotswap
    ctx_http.get("/lora-adapters", ex_wrapper(routes.get_lora_adapters));
    ctx_http.post("/lora-adapters", ex_wrapper(routes.post_lora_adapters));
    // Save & load slots
    ctx_http.get("/slots", ex_wrapper(routes.get_slots));
    ctx_http.post("/slots/:id_slot", ex_wrapper(routes.post_slots));

    //
    // Start the server
    //

    std::function<void()> clean_up;

    if (is_router_server) {
        LOG_INF("%s: starting router server, no model will be loaded in this process\n", __func__);

        clean_up = [&models_routes]() {
            SRV_INF("%s: cleaning up before exit...\n", __func__);
            if (models_routes.has_value()) {
                models_routes->models.unload_all();
            }
            llama_backend_free();
        };

        if (!ctx_http.start()) {
            clean_up();
            LOG_ERR("%s: exiting due to HTTP server error\n", __func__);
            return 1;
        }
        ctx_http.is_ready.store(true);

        shutdown_handler = [&](int) {
            ctx_http.stop();
        };

#if defined(LLAMA_DDS)
        // Start DDS polling thread for router mode (no model needed)
        if (dds_bridge && dds_running.load()) {
            std::string model_name = "router";
            // Fase 2: router mode has no model loaded, meta is null
            dds_poll_thread        = std::thread(dds_poll_loop, dds_bridge.get(), &ctx_server.get_queue(),
                                                 &ctx_server.get_response_queue(), ctx_server.get_vocab(), &dds_running,
                                                 model_name, static_cast<const server_context_meta *>(nullptr),
                                                 &params);  // Fase 2: common_params
            LOG_INF("%s: DDS polling thread started (router mode)\n", __func__);
        }
#endif

    } else {
        // setup clean up function, to be called before exit
        clean_up = [&ctx_http, &ctx_server]() {
            SRV_INF("%s: cleaning up before exit...\n", __func__);
            ctx_http.stop();
            ctx_server.terminate();
            llama_backend_free();
        };

        // start the HTTP server before loading the model to be able to serve /health requests
        if (!ctx_http.start()) {
            clean_up();
            LOG_ERR("%s: exiting due to HTTP server error\n", __func__);
            return 1;
        }

        // load the model
        LOG_INF("%s: loading model\n", __func__);

        if (!ctx_server.load_model(params)) {
            clean_up();
            if (ctx_http.thread.joinable()) {
                ctx_http.thread.join();
            }
            LOG_ERR("%s: exiting due to model loading error\n", __func__);
            return 1;
        }

        routes.update_meta(ctx_server);
        ctx_http.is_ready.store(true);

        LOG_INF("%s: model loaded\n", __func__);

#if defined(LLAMA_DDS)
        // A3: inform DDS bridge of the loaded model so status is accurate
        if (dds_bridge) {
            std::string model_name_str = params.model.name.empty() ? "unknown" : params.model.name;
            dds_bridge->set_model_info(model_name_str, true, params.n_parallel);
        }

        // Fase 2: snapshot meta – assignment to dds_meta declared in main() scope
        if (dds_bridge) {
            dds_meta = std::make_unique<server_context_meta>(ctx_server.get_meta());
        }

        // Start DDS polling thread after model is loaded
        if (dds_bridge && dds_running.load()) {
            // Get model name from params (fallback to param value)
            std::string model_name = params.model.name.empty() ? "unknown" : params.model.name;

            dds_poll_thread =
                std::thread(dds_poll_loop, dds_bridge.get(), &ctx_server.get_queue(), &ctx_server.get_response_queue(),
                            ctx_server.get_vocab(), &dds_running, model_name,
                            dds_meta.get(),  // Fase 2: real meta after load_model()
                            &params);        // Fase 2: common_params
            LOG_INF("%s: DDS polling thread started\n", __func__);
        }
#endif

        shutdown_handler = [&](int) {
            // this will unblock start_loop()
            ctx_server.terminate();
        };
    }

    // TODO: refactor in common/console
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
    struct sigaction sigint_action;
    sigint_action.sa_handler = signal_handler;
    sigemptyset(&sigint_action.sa_mask);
    sigint_action.sa_flags = 0;
    sigaction(SIGINT, &sigint_action, NULL);
    sigaction(SIGTERM, &sigint_action, NULL);
#elif defined(_WIN32)
    auto console_ctrl_handler = +[](DWORD ctrl_type) -> BOOL {
        return (ctrl_type == CTRL_C_EVENT) ? (signal_handler(SIGINT), true) : false;
    };
    SetConsoleCtrlHandler(reinterpret_cast<PHANDLER_ROUTINE>(console_ctrl_handler), true);
#endif

    if (is_router_server) {
        LOG_INF("%s: router server is listening on %s\n", __func__, ctx_http.listening_address.c_str());
        LOG_INF("%s: NOTE: router mode is experimental\n", __func__);
        LOG_INF("%s:       it is not recommended to use this mode in untrusted environments\n", __func__);
        if (ctx_http.thread.joinable()) {
            ctx_http.thread.join();  // keep the main thread alive
        }

        // when the HTTP server stops, clean up and exit
        clean_up();
    } else {
        LOG_INF("%s: server is listening on %s\n", __func__, ctx_http.listening_address.c_str());
        LOG_INF("%s: starting the main loop...\n", __func__);

        // optionally, notify router server that this instance is ready
        const char * router_port = std::getenv("LLAMA_SERVER_ROUTER_PORT");
        std::thread  monitor_thread;
        if (router_port != nullptr) {
            monitor_thread = server_models::setup_child_server(shutdown_handler);
        }

        // this call blocks the main thread until queue_tasks.terminate() is called
        ctx_server.start_loop();

#if defined(LLAMA_DDS)
        // Stop DDS polling thread
        if (dds_bridge) {
            dds_running = false;
            if (dds_poll_thread.joinable()) {
                dds_poll_thread.join();
            }
            dds_bridge->stop();
            LOG_INF("%s: DDS polling thread stopped\n", __func__);
        }
#endif

        clean_up();
        if (ctx_http.thread.joinable()) {
            ctx_http.thread.join();
        }
        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }

        auto * ll_ctx = ctx_server.get_llama_context();
        if (ll_ctx != nullptr) {
            llama_memory_breakdown_print(ll_ctx);
        }
    }

    return 0;
}
