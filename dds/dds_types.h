#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace llama_dds {

// Chat message structure
struct ChatMessage {
    std::string role;      // "system", "user", "assistant"
    std::string content;
};

// Chat completion request (maps to OpenAI API)
struct ChatCompletionRequest {
    std::string request_id;           // Unique UUID for correlation
    std::string model;
    std::vector<ChatMessage> messages;
    float temperature = 0.7f;
    int32_t max_tokens = 256;
    bool stream = false;
    std::optional<float> top_p;
    std::optional<int32_t> n;
    std::optional<std::vector<std::string>> stop;
};

// Chat completion response (each chunk for streaming)
struct ChatCompletionResponse {
    std::string request_id;           // Correlates with request
    std::string model;
    std::string content;               // Generated text
    std::optional<std::string> finish_reason;  // "stop", "length", null
    bool is_final = false;            // Last chunk?
    int32_t prompt_tokens = 0;
    int32_t completion_tokens = 0;
};

// Server health/status
struct ServerStatus {
    std::string server_id;
    int32_t slots_idle = 0;
    int32_t slots_processing = 0;
    std::string model_loaded;
    bool ready = false;
};

} // namespace llama_dds
