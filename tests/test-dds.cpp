#include "dds/dds_idl_wrapper.h"
#include <cassert>
#include <iostream>
#include <cstring>

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
    cpp_req.request_id = "test-id-123";
    cpp_req.model = "test-model";
    cpp_req.temperature = 0.7f;
    cpp_req.max_tokens = 100;
    cpp_req.stream = true;
    
    llama_dds::ChatMessage msg1;
    msg1.role = "user";
    msg1.content = "hello";
    cpp_req.messages.push_back(msg1);

    // Convert to IDL C type
    llama_ChatCompletionRequest c_req = llama_dds::to_llama_request(cpp_req);

    ASSERT(strcmp(c_req.request_id, "test-id-123") == 0);
    ASSERT(strcmp(c_req.model, "test-model") == 0);
    ASSERT(c_req.temperature == 0.7f);
    ASSERT(c_req.max_tokens == 100);
    ASSERT(c_req.stream == true);
    ASSERT(c_req.messages._length == 1);
    ASSERT(strcmp(c_req.messages._buffer[0].role, "user") == 0);
    ASSERT(strcmp(c_req.messages._buffer[0].content, "hello") == 0);

    // Convert back to C++ type
    llama_dds::ChatCompletionRequest cpp_req2 = llama_dds::to_request(c_req);

    ASSERT(cpp_req2.request_id == "test-id-123");
    ASSERT(cpp_req2.model == "test-model");
    ASSERT(cpp_req2.temperature == 0.7f);
    ASSERT(cpp_req2.max_tokens == 100);
    ASSERT(cpp_req2.stream == true);
    ASSERT(cpp_req2.messages.size() == 1);
    ASSERT(cpp_req2.messages[0].role == "user");
    ASSERT(cpp_req2.messages[0].content == "hello");

    // Cleanup
    llama_dds::free_llama_request(c_req);
    
    std::cout << "Request conversion passed." << std::endl;
}

void test_response_conversion() {
    std::cout << "Testing response conversion..." << std::endl;

    llama_dds::ChatCompletionResponse cpp_resp;
    cpp_resp.request_id = "req-123";
    cpp_resp.model = "gpt-4";
    cpp_resp.content = "world";
    cpp_resp.finish_reason = "stop";
    cpp_resp.is_final = true;
    cpp_resp.prompt_tokens = 10;
    cpp_resp.completion_tokens = 20;

    // Convert to IDL C type
    llama_ChatCompletionResponse c_resp = llama_dds::to_llama_response(cpp_resp);

    ASSERT(strcmp(c_resp.request_id, "req-123") == 0);
    ASSERT(strcmp(c_resp.model, "gpt-4") == 0);
    ASSERT(strcmp(c_resp.content, "world") == 0);
    ASSERT(strcmp(c_resp.finish_reason, "stop") == 0);
    ASSERT(c_resp.is_final == true);
    ASSERT(c_resp.prompt_tokens == 10);
    ASSERT(c_resp.completion_tokens == 20);

    // Convert back
    llama_dds::ChatCompletionResponse cpp_resp2 = llama_dds::to_response(c_resp);

    ASSERT(cpp_resp2.request_id == "req-123");
    ASSERT(cpp_resp2.model == "gpt-4");
    ASSERT(cpp_resp2.content == "world");
    ASSERT(cpp_resp2.finish_reason == "stop");
    ASSERT(cpp_resp2.is_final == true);
    ASSERT(cpp_resp2.prompt_tokens == 10);
    ASSERT(cpp_resp2.completion_tokens == 20);

    // Cleanup
    llama_dds::free_llama_response(c_resp);

    std::cout << "Response conversion passed." << std::endl;
}

int main() {
    test_request_conversion();
    test_response_conversion();
    return 0;
}
