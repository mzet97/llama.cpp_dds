#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llama_dds {

/// A single turn in a conversation.
struct ChatMessage {
    std::string role;  ///< "system", "user", or "assistant"
    std::string content;
};

/// LLM Inference Request (matches Python orchestrator's DDSLLMInferenceRequest).
/// Uses messages_json (JSON string) instead of structured ChatMessage sequence.
struct LLMInferenceRequest {
    std::string request_id;           ///< UUID for correlation
    std::string task_id;              ///< Task UUID from orchestrator
    std::string agent_id;             ///< Agent UUID that made the request
    std::string model_name;           ///< Model name (e.g., "qwen3.5-0.8b")
    std::string messages_json;        ///< JSON array of {role, content} objects
    float       temperature    = 0.7f;
    int32_t     max_tokens     = 512;
    bool        stream         = false;
    int32_t     security_level = 0;   ///< SecurityLevel enum (0=PUBLIC)
    std::string provider_constraint = "ANY";  ///< "ANY", "LOCAL_ONLY"
    uint64_t    created_at_ns  = 0;
};

/// LLM Inference Result (streaming chunks).
/// Matches Python orchestrator's DDSLLMInferenceResult.
struct LLMInferenceResult {
    std::string request_id;           ///< Correlates with request
    uint32_t    seq_num          = 0; ///< Sequence number for ordering
    std::string content;              ///< Generated text chunk
    bool        is_final         = false;
    int32_t     finish_reason    = 0; ///< FinishReason enum (0=NONE, 1=COMPLETION)
    std::string model_used;           ///< Model that generated the response
    uint32_t    tokens_prompt    = 0;
    uint32_t    tokens_completion = 0;
    uint64_t    emitted_at_ns    = 0;
};

/// LLM Inference Error.
/// Matches Python orchestrator's DDSLLMInferenceError.
struct LLMInferenceError {
    std::string request_id;           ///< Correlates with request
    int32_t     error_code       = 0;
    std::string error_message;
    std::string provider;
    bool        retriable        = false;
    uint64_t    emitted_at_ns    = 0;
};

/// Periodic heartbeat published by the server so clients can detect availability.
struct ServerStatus {
    std::string server_id;
    int32_t     slots_idle       = 0;
    int32_t     slots_processing = 0;
    std::string model_loaded;
    bool        ready = false;
};

// Legacy type aliases for backward compatibility
using ChatCompletionRequest  = LLMInferenceRequest;
using ChatCompletionResponse = LLMInferenceResult;

}  // namespace llama_dds
