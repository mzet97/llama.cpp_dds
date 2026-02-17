#pragma once

#include "dds_types.h"

#include "idl/LlamaDDS.h"

#include <dds/dds.h>
#include <vector>
#include <string>
#include <cstring>
#include <memory>
#include <cstdlib> // malloc, free

#ifdef _WIN32
#define dds_strdup _strdup
#else
#define dds_strdup strdup
#endif

namespace llama_dds {

// Helper to free memory allocated for llama_ChatCompletionRequest
inline void free_llama_request(llama_ChatCompletionRequest& req) {
    if (req.request_id) free(req.request_id);
    if (req.model) free(req.model);
    
    if (req.messages._buffer) {
        for (uint32_t i = 0; i < req.messages._length; i++) {
            if (req.messages._buffer[i].role) free(req.messages._buffer[i].role);
            if (req.messages._buffer[i].content) free(req.messages._buffer[i].content);
        }
        free(req.messages._buffer);
    }
    
    if (req.top_p._buffer) free(req.top_p._buffer);
    if (req.n._buffer) free(req.n._buffer);
    
    if (req.stop._buffer) {
        for (uint32_t i = 0; i < req.stop._length; i++) {
            if (req.stop._buffer[i]) free(req.stop._buffer[i]);
        }
        free(req.stop._buffer);
    }
}

// Helper to free memory allocated for llama_ChatCompletionResponse
inline void free_llama_response(llama_ChatCompletionResponse& resp) {
    if (resp.request_id) free(resp.request_id);
    if (resp.model) free(resp.model);
    if (resp.content) free(resp.content);
    if (resp.finish_reason) free(resp.finish_reason);
}

// Conversion helpers between C++ types and IDL C types

// ChatMessage
inline ChatMessage to_chat_message(const llama_ChatMessage& msg) {
    ChatMessage result;
    result.role = msg.role ? msg.role : "";
    result.content = msg.content ? msg.content : "";
    return result;
}

inline llama_ChatMessage to_llama_chat_message(const ChatMessage& msg) {
    llama_ChatMessage result;
    result.role = dds_strdup(msg.role.c_str());
    result.content = dds_strdup(msg.content.c_str());
    return result;
}

// ChatCompletionRequest
inline ChatCompletionRequest to_request(const llama_ChatCompletionRequest& req) {
    ChatCompletionRequest result;
    result.request_id = req.request_id ? req.request_id : "";
    result.model = req.model ? req.model : "";
    result.temperature = req.temperature;
    result.max_tokens = req.max_tokens;
    result.stream = req.stream;

    // Convert messages
    for (uint32_t i = 0; i < req.messages._length; i++) {
        result.messages.push_back(to_chat_message(req.messages._buffer[i]));
    }

    // Optional: top_p
    if (req.top_p._length > 0) {
        result.top_p = req.top_p._buffer[0];
    }

    // Optional: n
    if (req.n._length > 0) {
        result.n = req.n._buffer[0];
    }

    // Optional: stop
    if (req.stop._length > 0) {
        result.stop = std::vector<std::string>();
        for (uint32_t i = 0; i < req.stop._length; i++) {
            result.stop->push_back(req.stop._buffer[i] ? req.stop._buffer[i] : "");
        }
    }

    return result;
}

inline llama_ChatCompletionRequest to_llama_request(const ChatCompletionRequest& req) {
    llama_ChatCompletionRequest result;
    memset(&result, 0, sizeof(result)); // Initialize to zero
    
    result.request_id = dds_strdup(req.request_id.c_str());
    result.model = dds_strdup(req.model.c_str());
    result.temperature = req.temperature;
    result.max_tokens = req.max_tokens;
    result.stream = req.stream;

    // Messages
    result.messages._maximum = req.messages.size();
    result.messages._length = req.messages.size();
    result.messages._buffer = (llama_ChatMessage*)malloc(sizeof(llama_ChatMessage) * req.messages.size());
    result.messages._release = true;
    for (size_t i = 0; i < req.messages.size(); i++) {
        result.messages._buffer[i] = to_llama_chat_message(req.messages[i]);
    }

    // top_p
    if (req.top_p) {
        result.top_p._maximum = 1;
        result.top_p._length = 1;
        result.top_p._buffer = (float*)malloc(sizeof(float));
        result.top_p._buffer[0] = *req.top_p;
        result.top_p._release = true;
    } else {
        result.top_p._maximum = 0;
        result.top_p._length = 0;
        result.top_p._buffer = nullptr;
        result.top_p._release = false;
    }

    // n
    if (req.n) {
        result.n._maximum = 1;
        result.n._length = 1;
        result.n._buffer = (int32_t*)malloc(sizeof(int32_t));
        result.n._buffer[0] = *req.n;
        result.n._release = true;
    } else {
        result.n._maximum = 0;
        result.n._length = 0;
        result.n._buffer = nullptr;
        result.n._release = false;
    }

    // stop
    if (req.stop) {
        result.stop._maximum = req.stop->size();
        result.stop._length = req.stop->size();
        result.stop._buffer = (char**)malloc(sizeof(char*) * req.stop->size());
        result.stop._release = true;
        for (size_t i = 0; i < req.stop->size(); i++) {
            result.stop._buffer[i] = dds_strdup(req.stop->at(i).c_str());
        }
    } else {
        result.stop._maximum = 0;
        result.stop._length = 0;
        result.stop._buffer = nullptr;
        result.stop._release = false;
    }

    return result;
}

// ChatCompletionResponse
inline ChatCompletionResponse to_response(const llama_ChatCompletionResponse& resp) {
    ChatCompletionResponse result;
    result.request_id = resp.request_id ? resp.request_id : "";
    result.model = resp.model ? resp.model : "";
    result.content = resp.content ? resp.content : "";
    result.finish_reason = resp.finish_reason ? std::make_optional<std::string>(resp.finish_reason) : std::nullopt;
    result.is_final = resp.is_final;
    result.prompt_tokens = resp.prompt_tokens;
    result.completion_tokens = resp.completion_tokens;
    return result;
}

inline llama_ChatCompletionResponse to_llama_response(const ChatCompletionResponse& resp) {
    llama_ChatCompletionResponse result;
    memset(&result, 0, sizeof(result));
    
    result.request_id = dds_strdup(resp.request_id.c_str());
    result.model = dds_strdup(resp.model.c_str());
    result.content = dds_strdup(resp.content.c_str());
    result.finish_reason = resp.finish_reason ? dds_strdup(resp.finish_reason->c_str()) : nullptr;
    result.is_final = resp.is_final;
    result.prompt_tokens = resp.prompt_tokens;
    result.completion_tokens = resp.completion_tokens;
    return result;
}

// ServerStatus
inline ServerStatus to_status(const llama_ServerStatus& status) {
    ServerStatus result;
    result.server_id = status.server_id ? status.server_id : "";
    result.slots_idle = status.slots_idle;
    result.slots_processing = status.slots_processing;
    result.model_loaded = status.model_loaded ? status.model_loaded : "";
    result.ready = status.ready;
    return result;
}

inline llama_ServerStatus to_llama_status(const ServerStatus& status) {
    llama_ServerStatus result;
    memset(&result, 0, sizeof(result));
    
    result.server_id = dds_strdup(status.server_id.c_str());
    result.slots_idle = status.slots_idle;
    result.slots_processing = status.slots_processing;
    result.model_loaded = dds_strdup(status.model_loaded.c_str());
    result.ready = status.ready;
    return result;
}

} // namespace llama_dds
