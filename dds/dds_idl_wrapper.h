#pragma once

#include "dds_types.h"
#include "idl/OrchestratorDDS.h"

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

// ============================================================
// Memory cleanup helpers
// ============================================================

inline void free_llm_request(orchestrator_LLMInferenceRequest & req) {
    free(req.request_id);          req.request_id          = nullptr;
    free(req.task_id);             req.task_id             = nullptr;
    free(req.agent_id);            req.agent_id            = nullptr;
    free(req.model_name);          req.model_name          = nullptr;
    free(req.messages_json);       req.messages_json       = nullptr;
    free(req.provider_constraint); req.provider_constraint = nullptr;
}

inline void free_llm_result(orchestrator_LLMInferenceResult & res) {
    free(res.request_id);  res.request_id  = nullptr;
    free(res.content);     res.content     = nullptr;
    free(res.model_used);  res.model_used  = nullptr;
}

inline void free_llm_error(orchestrator_LLMInferenceError & err) {
    free(err.request_id);   err.request_id   = nullptr;
    free(err.error_message); err.error_message = nullptr;
    free(err.provider);      err.provider      = nullptr;
}

inline void free_server_status(orchestrator_ServerStatus & status) {
    free(status.server_id);    status.server_id    = nullptr;
    free(status.model_loaded); status.model_loaded = nullptr;
}

// Legacy aliases
inline void free_llama_request(orchestrator_LLMInferenceRequest & req) { free_llm_request(req); }
inline void free_llama_response(orchestrator_LLMInferenceResult & res) { free_llm_result(res); }
inline void free_llama_status(orchestrator_ServerStatus & status) { free_server_status(status); }

// ============================================================
// IDL C -> C++ conversion
// ============================================================

inline LLMInferenceRequest to_request(const orchestrator_LLMInferenceRequest & req) {
    LLMInferenceRequest result;
    result.request_id         = req.request_id ? req.request_id : "";
    result.task_id            = req.task_id ? req.task_id : "";
    result.agent_id           = req.agent_id ? req.agent_id : "";
    result.model_name         = req.model_name ? req.model_name : "";
    result.messages_json      = req.messages_json ? req.messages_json : "[]";
    result.temperature        = req.temperature;
    result.max_tokens         = req.max_tokens;
    result.stream             = req.stream;
    result.security_level     = req.security_level;
    result.provider_constraint = req.provider_constraint ? req.provider_constraint : "ANY";
    result.created_at_ns      = req.created_at_ns;
    return result;
}

inline LLMInferenceResult to_result(const orchestrator_LLMInferenceResult & res) {
    LLMInferenceResult result;
    result.request_id        = res.request_id ? res.request_id : "";
    result.seq_num           = res.seq_num;
    result.content           = res.content ? res.content : "";
    result.is_final          = res.is_final;
    result.finish_reason     = res.finish_reason;
    result.model_used        = res.model_used ? res.model_used : "";
    result.tokens_prompt     = res.tokens_prompt;
    result.tokens_completion = res.tokens_completion;
    result.emitted_at_ns     = res.emitted_at_ns;
    return result;
}

inline LLMInferenceError to_error(const orchestrator_LLMInferenceError & err) {
    LLMInferenceError result;
    result.request_id    = err.request_id ? err.request_id : "";
    result.error_code    = err.error_code;
    result.error_message = err.error_message ? err.error_message : "";
    result.provider      = err.provider ? err.provider : "";
    result.retriable     = err.retriable;
    result.emitted_at_ns = err.emitted_at_ns;
    return result;
}

inline ServerStatus to_status(const orchestrator_ServerStatus & status) {
    ServerStatus result;
    result.server_id        = status.server_id ? status.server_id : "";
    result.slots_idle       = status.slots_idle;
    result.slots_processing = status.slots_processing;
    result.model_loaded     = status.model_loaded ? status.model_loaded : "";
    result.ready            = status.ready;
    return result;
}

// ============================================================
// C++ -> IDL C conversion
// ============================================================

inline orchestrator_LLMInferenceRequest to_idl_request(const LLMInferenceRequest & req) {
    orchestrator_LLMInferenceRequest result;
    memset(&result, 0, sizeof(result));

    result.request_id         = dds_strdup(req.request_id.c_str());
    result.task_id            = dds_strdup(req.task_id.c_str());
    result.agent_id           = dds_strdup(req.agent_id.c_str());
    result.model_name         = dds_strdup(req.model_name.c_str());
    result.messages_json      = dds_strdup(req.messages_json.c_str());
    result.provider_constraint = dds_strdup(req.provider_constraint.c_str());

    if (!result.request_id || !result.task_id || !result.agent_id ||
        !result.model_name || !result.messages_json || !result.provider_constraint) {
        fprintf(stderr, "[DDS] OOM: dds_strdup failed in to_idl_request\n");
        free_llm_request(result);
        memset(&result, 0, sizeof(result));
        return result;
    }

    result.temperature     = req.temperature;
    result.max_tokens      = req.max_tokens;
    result.stream          = req.stream;
    result.security_level  = req.security_level;
    result.created_at_ns   = req.created_at_ns;
    return result;
}

inline orchestrator_LLMInferenceResult to_idl_result(const LLMInferenceResult & res) {
    orchestrator_LLMInferenceResult result;
    memset(&result, 0, sizeof(result));

    result.request_id = dds_strdup(res.request_id.c_str());
    result.content    = dds_strdup(res.content.c_str());
    result.model_used = dds_strdup(res.model_used.c_str());

    if (!result.request_id || !result.content || !result.model_used) {
        fprintf(stderr, "[DDS] OOM: dds_strdup failed in to_idl_result\n");
        free_llm_result(result);
        memset(&result, 0, sizeof(result));
        return result;
    }

    result.seq_num           = res.seq_num;
    result.is_final          = res.is_final;
    result.finish_reason     = res.finish_reason;
    result.tokens_prompt     = res.tokens_prompt;
    result.tokens_completion = res.tokens_completion;
    result.emitted_at_ns     = res.emitted_at_ns;
    return result;
}

inline orchestrator_LLMInferenceError to_idl_error(const LLMInferenceError & err) {
    orchestrator_LLMInferenceError result;
    memset(&result, 0, sizeof(result));

    result.request_id    = dds_strdup(err.request_id.c_str());
    result.error_message = dds_strdup(err.error_message.c_str());
    result.provider      = dds_strdup(err.provider.c_str());

    if (!result.request_id || !result.error_message || !result.provider) {
        fprintf(stderr, "[DDS] OOM: dds_strdup failed in to_idl_error\n");
        free_llm_error(result);
        memset(&result, 0, sizeof(result));
        return result;
    }

    result.error_code    = err.error_code;
    result.retriable     = err.retriable;
    result.emitted_at_ns = err.emitted_at_ns;
    return result;
}

inline orchestrator_ServerStatus to_idl_status(const ServerStatus & status) {
    orchestrator_ServerStatus result;
    memset(&result, 0, sizeof(result));

    result.server_id    = dds_strdup(status.server_id.c_str());
    result.model_loaded = dds_strdup(status.model_loaded.c_str());

    if (!result.server_id || !result.model_loaded) {
        fprintf(stderr, "[DDS] OOM: dds_strdup failed in to_idl_status\n");
        free_server_status(result);
        memset(&result, 0, sizeof(result));
        return result;
    }

    result.slots_idle       = status.slots_idle;
    result.slots_processing = status.slots_processing;
    result.ready            = status.ready;
    return result;
}

// ============================================================
// Legacy aliases (for backward compatibility with existing code)
// ============================================================

using llama_ChatCompletionRequest  = orchestrator_LLMInferenceRequest;
using llama_ChatCompletionResponse = orchestrator_LLMInferenceResult;
using llama_ServerStatus           = orchestrator_ServerStatus;

inline ChatCompletionRequest to_request_legacy(const llama_ChatCompletionRequest & req) {
    return to_request(req);
}

inline llama_ChatCompletionRequest to_llama_request(const ChatCompletionRequest & req) {
    return to_idl_request(req);
}

inline ChatCompletionResponse to_response(const llama_ChatCompletionResponse & resp) {
    return to_result(resp);
}

inline llama_ChatCompletionResponse to_llama_response(const ChatCompletionResponse & resp) {
    return to_idl_result(resp);
}

inline llama_ServerStatus to_llama_status(const ServerStatus & status) {
    return to_idl_status(status);
}

}  // namespace llama_dds
