#include "dds/dds_idl_wrapper.h"
#include "dds/dds_transport.h"
#include "dds/dds_bridge.h"
#include <dds/dds.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <cstring>
#include <thread>

using namespace llama_dds;

// Simple test runner
#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while (0)

void test_request_conversion() {
    std::cout << "Testing request conversion..." << std::endl;

    llama_dds::ChatCompletionRequest cpp_req;
    cpp_req.request_id    = "test-id-123";
    cpp_req.model_name    = "test-model";
    cpp_req.temperature   = 0.7f;
    cpp_req.max_tokens    = 100;
    cpp_req.stream        = true;
    cpp_req.messages_json = R"([{"role":"user","content":"hello"}])";

    // Convert to IDL C type
    llama_ChatCompletionRequest c_req = llama_dds::to_llama_request(cpp_req);

    ASSERT(strcmp(c_req.request_id, "test-id-123") == 0);
    ASSERT(strcmp(c_req.model_name, "test-model") == 0);
    ASSERT(c_req.temperature == 0.7f);
    ASSERT(c_req.max_tokens == 100);
    ASSERT(c_req.stream == true);
    ASSERT(strcmp(c_req.messages_json, R"([{"role":"user","content":"hello"}])") == 0);

    // Convert back to C++ type
    llama_dds::ChatCompletionRequest cpp_req2 = llama_dds::to_request(c_req);

    ASSERT(cpp_req2.request_id == "test-id-123");
    ASSERT(cpp_req2.model_name == "test-model");
    ASSERT(cpp_req2.temperature == 0.7f);
    ASSERT(cpp_req2.max_tokens == 100);
    ASSERT(cpp_req2.stream == true);
    ASSERT(cpp_req2.messages_json == R"([{"role":"user","content":"hello"}])");

    // Cleanup
    llama_dds::free_llama_request(c_req);

    std::cout << "Request conversion passed." << std::endl;
}

void test_response_conversion() {
    std::cout << "Testing response conversion..." << std::endl;

    llama_dds::ChatCompletionResponse cpp_resp;
    cpp_resp.request_id        = "req-123";
    cpp_resp.model_used        = "gpt-4";
    cpp_resp.content           = "world";
    cpp_resp.finish_reason     = 1;  // FinishReason::Completion (see orch-common)
    cpp_resp.is_final          = true;
    cpp_resp.tokens_prompt     = 10;
    cpp_resp.tokens_completion = 20;

    // Convert to IDL C type
    llama_ChatCompletionResponse c_resp = llama_dds::to_llama_response(cpp_resp);

    ASSERT(strcmp(c_resp.request_id, "req-123") == 0);
    ASSERT(strcmp(c_resp.model_used, "gpt-4") == 0);
    ASSERT(strcmp(c_resp.content, "world") == 0);
    ASSERT(c_resp.finish_reason == 1);
    ASSERT(c_resp.is_final == true);
    ASSERT(c_resp.tokens_prompt == 10);
    ASSERT(c_resp.tokens_completion == 20);

    // Convert back
    llama_dds::ChatCompletionResponse cpp_resp2 = llama_dds::to_response(c_resp);

    ASSERT(cpp_resp2.request_id == "req-123");
    ASSERT(cpp_resp2.model_used == "gpt-4");
    ASSERT(cpp_resp2.content == "world");
    ASSERT(cpp_resp2.finish_reason == 1);
    ASSERT(cpp_resp2.is_final == true);
    ASSERT(cpp_resp2.tokens_prompt == 10);
    ASSERT(cpp_resp2.tokens_completion == 20);

    // Cleanup
    llama_dds::free_llama_response(c_resp);

    std::cout << "Response conversion passed." << std::endl;
}

void test_error_publication() {
    const int domain_id = 219;
    dds_entity_t participant = dds_create_participant(domain_id, nullptr, nullptr);
    ASSERT(participant >= 0);

    dds_entity_t topic = dds_create_topic(participant, &orchestrator_LLMInferenceError_desc,
                                          "LLM.InferenceError", nullptr, nullptr);
    ASSERT(topic >= 0);

    dds_qos_t * qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);
    dds_entity_t reader = dds_create_reader(participant, topic, qos, nullptr);
    dds_delete_qos(qos);
    ASSERT(reader >= 0);

    DDSTransport transport(domain_id);
    ASSERT(transport.start_server([](const ChatCompletionRequest &) {}));

    LLMInferenceError expected;
    expected.request_id = "test-error-publication";
    expected.error_code = 500;
    expected.error_message = "simulated failure";
    expected.provider = "llama.cpp-dds";
    expected.retriable = false;
    transport.send_error(expected);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool received = false;
    while (std::chrono::steady_clock::now() < deadline && !received) {
        void * samples[1] = { nullptr };
        dds_sample_info_t infos[1];
        const dds_return_t n = dds_take(reader, samples, infos, 1, 1);
        if (n > 0) {
            if (infos[0].valid_data) {
                const auto * sample = static_cast<orchestrator_LLMInferenceError *>(samples[0]);
                const auto actual = to_error(*sample);
                if (actual.request_id == expected.request_id) {
                    ASSERT(actual.error_code == expected.error_code);
                    ASSERT(actual.error_message == expected.error_message);
                    ASSERT(actual.provider == expected.provider);
                    ASSERT(actual.retriable == expected.retriable);
                    ASSERT(actual.emitted_at_ns > 0);
                    received = true;
                }
            }
            dds_return_loan(reader, samples, n);
        }
    }

    transport.stop_server();
    dds_delete(participant);
    ASSERT(received);
}

// BRIDGE-STATE-013: cada request tem exatamente UMA transição terminal —
// final duplicado ou cancel tardio não podem liberar capacidade alheia; e um
// retry legítimo (mesmo request_id re-enfileirado) volta a contar/decrementar.
void test_bridge_terminal_idempotency() {
    std::cout << "Testing bridge terminal idempotency..." << std::endl;

    const int domain_id = 220;
    DDSBridge bridge(domain_id);
    ASSERT(bridge.init());
    ASSERT(bridge.start());

    DDSTransport client(domain_id);
    client.subscribe_responses([](const ChatCompletionResponse &) {});
    ASSERT(client.start_client());

    ChatCompletionRequest req;
    req.request_id    = "req-terminal-1";
    req.model_name    = "test-model";
    req.messages_json = R"([{"role":"user","content":"hi"}])";
    req.max_tokens    = 4;
    req.stream        = true;

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // settle/match
    client.send_request(req);

    ChatCompletionRequest got;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (bridge.pop_pending_request(got)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT(got.request_id == "req-terminal-1");
    ASSERT(bridge.pending_count() == 1);

    ChatCompletionResponse resp;
    resp.request_id = "req-terminal-1";
    resp.model_used = "test-model";
    resp.content    = "done";
    resp.is_final   = true;

    bridge.send_response(resp);                       // terminal (decrementa)
    bridge.send_response(resp);                       // duplicado — no-op
    bridge.cancel_pending_request("req-terminal-1");  // tardio — no-op
    ASSERT(bridge.pending_count() == 0);

    // Retry legítimo: mesmo request_id re-enfileirado conta e decrementa de novo.
    client.send_request(req);
    got = ChatCompletionRequest{};
    const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline2) {
        if (bridge.pop_pending_request(got)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT(got.request_id == "req-terminal-1");
    ASSERT(bridge.pending_count() == 1);
    bridge.send_response(resp);
    ASSERT(bridge.pending_count() == 0);

    client.stop_client();
    bridge.stop();
    std::cout << "Bridge terminal idempotency passed." << std::endl;
}

int main() {
    test_request_conversion();
    test_response_conversion();
    test_error_publication();
    test_bridge_terminal_idempotency();
    return 0;
}