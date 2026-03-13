#pragma once

#include "dds_types.h"
#include "idl/LlamaDDS.h"

#include <dds/dds.h>

#include <cstdlib>  // malloc, free
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#    define dds_strdup _strdup
#else
#    define dds_strdup strdup
#endif

namespace llama_dds {

// Helper to free memory allocated for llama_ChatCompletionRequest
inline void free_llama_request(llama_ChatCompletionRequest & req) {
    if (req.request_id) {
        free(req.request_id);
        req.request_id = nullptr;
    }
    if (req.model) {
        free(req.model);
        req.model = nullptr;
    }

    if (req.messages._buffer) {
        for (uint32_t i = 0; i < req.messages._length; i++) {
            if (req.messages._buffer[i].role) {
                free(req.messages._buffer[i].role);
                req.messages._buffer[i].role = nullptr;
            }
            if (req.messages._buffer[i].content) {
                free(req.messages._buffer[i].content);
                req.messages._buffer[i].content = nullptr;
            }
        }
        free(req.messages._buffer);
        req.messages._buffer = nullptr;
    }

    if (req.top_p._buffer) {
        free(req.top_p._buffer);
        req.top_p._buffer = nullptr;
    }
    if (req.n._buffer) {
        free(req.n._buffer);
        req.n._buffer = nullptr;
    }

    if (req.stop._buffer) {
        for (uint32_t i = 0; i < req.stop._length; i++) {
            if (req.stop._buffer[i]) {
                free(req.stop._buffer[i]);
                req.stop._buffer[i] = nullptr;
            }
        }
        free(req.stop._buffer);
        req.stop._buffer = nullptr;
    }
}

// Helper to free memory allocated for llama_ChatCompletionResponse
inline void free_llama_response(llama_ChatCompletionResponse & resp) {
    if (resp.request_id) {
        free(resp.request_id);
        resp.request_id = nullptr;
    }
    if (resp.model) {
        free(resp.model);
        resp.model = nullptr;
    }
    if (resp.content) {
        free(resp.content);
        resp.content = nullptr;
    }
    if (resp.finish_reason) {
        free(resp.finish_reason);
        resp.finish_reason = nullptr;
    }
}

// Helper to free memory allocated for llama_ServerStatus
inline void free_llama_status(llama_ServerStatus & status) {
    if (status.server_id) {
        free(status.server_id);
    }
    if (status.model_loaded) {
        free(status.model_loaded);
    }
    status.server_id    = nullptr;
    status.model_loaded = nullptr;
}

// Conversion helpers between C++ types and IDL C types

// ChatMessage
inline ChatMessage to_chat_message(const llama_ChatMessage & msg) {
    ChatMessage result;
    result.role    = msg.role ? msg.role : "";
    result.content = msg.content ? msg.content : "";
    return result;
}

inline llama_ChatMessage to_llama_chat_message(const ChatMessage & msg) {
    llama_ChatMessage result;
    memset(&result, 0, sizeof(result));
    result.role    = dds_strdup(msg.role.c_str());
    result.content = dds_strdup(msg.content.c_str());
    if (!result.role || !result.content) {
        fprintf(stderr, "[DDS] OOM: dds_strdup failed in to_llama_chat_message\n");
        free(result.role);
        free(result.content);
        memset(&result, 0, sizeof(result));
    }
    return result;
}

// ChatCompletionRequest
inline ChatCompletionRequest to_request(const llama_ChatCompletionRequest & req) {
    ChatCompletionRequest result;
    result.request_id  = req.request_id ? req.request_id : "";
    result.model       = req.model ? req.model : "";
    result.temperature = req.temperature;
    result.max_tokens  = req.max_tokens;
    result.stream      = req.stream;

    // Convert messages
    if (req.messages._buffer != nullptr) {
        for (uint32_t i = 0; i < req.messages._length; i++) {
            result.messages.push_back(to_chat_message(req.messages._buffer[i]));
        }
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

inline llama_ChatCompletionRequest to_llama_request(const ChatCompletionRequest & req) {
    llama_ChatCompletionRequest result;
    memset(&result, 0, sizeof(result));  // Initialize to zero

    result.request_id  = dds_strdup(req.request_id.c_str());
    result.model       = dds_strdup(req.model.c_str());
    if (!result.request_id || !result.model) {
        fprintf(stderr, "[DDS] OOM: dds_strdup failed in to_llama_request\n");
        free(result.request_id);
        free(result.model);
        memset(&result, 0, sizeof(result));
        return result;
    }
    result.temperature = req.temperature;
    result.max_tokens  = req.max_tokens;
    result.stream      = req.stream;

    // Messages
    if (req.messages.empty()) {
        result.messages._maximum = 0;
        result.messages._length  = 0;
        result.messages._buffer  = nullptr;
        result.messages._release = false;
    } else {
        result.messages._maximum = req.messages.size();
        result.messages._length  = req.messages.size();
        result.messages._buffer  = (llama_ChatMessage *) malloc(sizeof(llama_ChatMessage) * req.messages.size());
        if (!result.messages._buffer) {
            fprintf(stderr, "[DDS] OOM: failed to allocate messages buffer (count=%zu)\n", req.messages.size());
            free_llama_request(result);
            memset(&result, 0, sizeof(result));
            return result;
        }
        result.messages._release = true;
        for (size_t i = 0; i < req.messages.size(); i++) {
            result.messages._buffer[i] = to_llama_chat_message(req.messages[i]);
            if (!result.messages._buffer[i].role || !result.messages._buffer[i].content) {
                // OOM during message conversion — clean up everything
                fprintf(stderr, "[DDS] OOM: to_llama_chat_message failed at index %zu\n", i);
                result.messages._length = (uint32_t)(i);  // only free successfully allocated messages
                free_llama_request(result);
                memset(&result, 0, sizeof(result));
                return result;
            }
        }
    }

    // top_p
    if (req.top_p) {
        result.top_p._maximum   = 1;
        result.top_p._length    = 1;
        result.top_p._buffer    = (float *) malloc(sizeof(float));
        if (result.top_p._buffer) {
            result.top_p._buffer[0] = *req.top_p;
            result.top_p._release   = true;
        } else {
            fprintf(stderr, "[DDS] OOM: failed to allocate top_p buffer\n");
            free_llama_request(result);
            memset(&result, 0, sizeof(result));
            return result;
        }
    } else {
        result.top_p._maximum = 0;
        result.top_p._length  = 0;
        result.top_p._buffer  = nullptr;
        result.top_p._release = false;
    }

    // n
    if (req.n) {
        result.n._maximum   = 1;
        result.n._length    = 1;
        result.n._buffer    = (int32_t *) malloc(sizeof(int32_t));
        if (result.n._buffer) {
            result.n._buffer[0] = *req.n;
            result.n._release   = true;
        } else {
            fprintf(stderr, "[DDS] OOM: failed to allocate n buffer\n");
            free_llama_request(result);
            memset(&result, 0, sizeof(result));
            return result;
        }
    } else {
        result.n._maximum = 0;
        result.n._length  = 0;
        result.n._buffer  = nullptr;
        result.n._release = false;
    }

    // stop
    if (req.stop) {
        result.stop._maximum = req.stop->size();
        result.stop._length  = req.stop->size();
        result.stop._buffer  = (char **) malloc(sizeof(char *) * req.stop->size());
        if (result.stop._buffer) {
            result.stop._release = true;
            for (size_t i = 0; i < req.stop->size(); i++) {
                result.stop._buffer[i] = dds_strdup(req.stop->at(i).c_str());
                if (!result.stop._buffer[i]) {
                    fprintf(stderr, "[DDS] OOM: dds_strdup failed for stop string %zu\n", i);
                    result.stop._length = (uint32_t) i;
                    free_llama_request(result);
                    memset(&result, 0, sizeof(result));
                    return result;
                }
            }
        } else {
            result.stop._maximum = 0;
            result.stop._length  = 0;
            result.stop._release = false;
        }
    } else {
        result.stop._maximum = 0;
        result.stop._length  = 0;
        result.stop._buffer  = nullptr;
        result.stop._release = false;
    }

    return result;
}

// ChatCompletionResponse
inline ChatCompletionResponse to_response(const llama_ChatCompletionResponse & resp) {
    ChatCompletionResponse result;
    result.request_id        = resp.request_id ? resp.request_id : "";
    result.model             = resp.model ? resp.model : "";
    result.content           = resp.content ? resp.content : "";
    // Empty string on wire represents nullopt (IDL strings cannot be null)
    result.finish_reason     = (resp.finish_reason && resp.finish_reason[0] != '\0')
                                 ? std::make_optional<std::string>(resp.finish_reason)
                                 : std::nullopt;
    result.is_final          = resp.is_final;
    result.prompt_tokens     = resp.prompt_tokens;
    result.completion_tokens = resp.completion_tokens;
    return result;
}

inline llama_ChatCompletionResponse to_llama_response(const ChatCompletionResponse & resp) {
    llama_ChatCompletionResponse result;
    memset(&result, 0, sizeof(result));

    result.request_id    = dds_strdup(resp.request_id.c_str());
    result.model         = dds_strdup(resp.model.c_str());
    result.content       = dds_strdup(resp.content.c_str());
    // IDL requires non-null string; use empty string to represent "absent"
    result.finish_reason = resp.finish_reason ? dds_strdup(resp.finish_reason->c_str()) : dds_strdup("");

    if (!result.request_id || !result.model || !result.content || !result.finish_reason) {
        fprintf(stderr, "[DDS] OOM: dds_strdup failed in to_llama_response\n");
        // Free any successful allocations
        free(result.request_id);
        free(result.model);
        free(result.content);
        free(result.finish_reason);
        memset(&result, 0, sizeof(result));
        return result;
    }

    result.is_final          = resp.is_final;
    result.prompt_tokens     = resp.prompt_tokens;
    result.completion_tokens = resp.completion_tokens;
    return result;
}

// ServerStatus
inline ServerStatus to_status(const llama_ServerStatus & status) {
    ServerStatus result;
    result.server_id        = status.server_id ? status.server_id : "";
    result.slots_idle       = status.slots_idle;
    result.slots_processing = status.slots_processing;
    result.model_loaded     = status.model_loaded ? status.model_loaded : "";
    result.ready            = status.ready;
    return result;
}

inline llama_ServerStatus to_llama_status(const ServerStatus & status) {
    llama_ServerStatus result;
    memset(&result, 0, sizeof(result));

    result.server_id    = dds_strdup(status.server_id.c_str());
    result.model_loaded = dds_strdup(status.model_loaded.c_str());

    if (!result.server_id || !result.model_loaded) {
        fprintf(stderr, "[DDS] OOM: dds_strdup failed in to_llama_status\n");
        free(result.server_id);
        free(result.model_loaded);
        memset(&result, 0, sizeof(result));
        return result;
    }

    result.slots_idle       = status.slots_idle;
    result.slots_processing = status.slots_processing;
    result.ready            = status.ready;
    return result;
}

}  // namespace llama_dds
