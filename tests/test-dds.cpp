#include "dds/dds_idl_wrapper.h"
#include <cassert>
#include <iostream>
#include <cstring>

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

int main() {
    test_request_conversion();
    test_response_conversion();
    return 0;
}
