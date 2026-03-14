#pragma once

// Conversions between llama_dds::* C++ domain types and llama_grpc::* protobuf types.
// Much simpler than dds_idl_wrapper.h — protobuf uses std::string natively,
// no malloc/free or null-pointer concerns.

#include "dds_types.h"
#include "llama_service.pb.h"

namespace llama_grpc {

// ── ChatCompletionRequest ────────────────────────────────────────────

inline llama_dds::ChatCompletionRequest from_proto(const llama_grpc::ChatCompletionRequest & proto) {
    llama_dds::ChatCompletionRequest req;
    req.request_id  = proto.request_id();
    req.model       = proto.model();
    req.temperature = proto.temperature();
    req.max_tokens  = proto.max_tokens();
    req.stream      = proto.stream();

    req.messages.reserve(proto.messages_size());
    for (const auto & m : proto.messages()) {
        req.messages.push_back({ m.role(), m.content() });
    }

    if (proto.has_top_p()) {
        req.top_p = proto.top_p();
    }
    if (proto.has_n()) {
        req.n = proto.n();
    }
    if (proto.stop_size() > 0) {
        std::vector<std::string> stops;
        stops.reserve(proto.stop_size());
        for (const auto & s : proto.stop()) {
            stops.push_back(s);
        }
        req.stop = std::move(stops);
    }

    return req;
}

inline llama_grpc::ChatCompletionRequest to_proto(const llama_dds::ChatCompletionRequest & req) {
    llama_grpc::ChatCompletionRequest proto;
    proto.set_request_id(req.request_id);
    proto.set_model(req.model);
    proto.set_temperature(req.temperature);
    proto.set_max_tokens(req.max_tokens);
    proto.set_stream(req.stream);

    for (const auto & m : req.messages) {
        auto * msg = proto.add_messages();
        msg->set_role(m.role);
        msg->set_content(m.content);
    }

    if (req.top_p.has_value()) {
        proto.set_top_p(req.top_p.value());
    }
    if (req.n.has_value()) {
        proto.set_n(req.n.value());
    }
    if (req.stop.has_value()) {
        for (const auto & s : req.stop.value()) {
            proto.add_stop(s);
        }
    }

    return proto;
}

// ── ChatCompletionResponse ───────────────────────────────────────────

inline llama_dds::ChatCompletionResponse from_proto(const llama_grpc::ChatCompletionResponse & proto) {
    llama_dds::ChatCompletionResponse resp;
    resp.request_id        = proto.request_id();
    resp.model             = proto.model();
    resp.content           = proto.content();
    resp.is_final          = proto.is_final();
    resp.prompt_tokens     = proto.prompt_tokens();
    resp.completion_tokens = proto.completion_tokens();

    // Empty string in protobuf means "not set" (mirrors DDS IDL convention)
    if (!proto.finish_reason().empty()) {
        resp.finish_reason = proto.finish_reason();
    }

    return resp;
}

inline llama_grpc::ChatCompletionResponse to_proto(const llama_dds::ChatCompletionResponse & resp) {
    llama_grpc::ChatCompletionResponse proto;
    proto.set_request_id(resp.request_id);
    proto.set_model(resp.model);
    proto.set_content(resp.content);
    proto.set_finish_reason(resp.finish_reason.value_or(""));
    proto.set_is_final(resp.is_final);
    proto.set_prompt_tokens(resp.prompt_tokens);
    proto.set_completion_tokens(resp.completion_tokens);
    return proto;
}

// ── ServerStatus ─────────────────────────────────────────────────────

inline llama_dds::ServerStatus from_proto(const llama_grpc::ServerStatus & proto) {
    llama_dds::ServerStatus status;
    status.server_id        = proto.server_id();
    status.slots_idle       = proto.slots_idle();
    status.slots_processing = proto.slots_processing();
    status.model_loaded     = proto.model_loaded();
    status.ready            = proto.ready();
    return status;
}

inline llama_grpc::ServerStatus to_proto(const llama_dds::ServerStatus & status) {
    llama_grpc::ServerStatus proto;
    proto.set_server_id(status.server_id);
    proto.set_slots_idle(status.slots_idle);
    proto.set_slots_processing(status.slots_processing);
    proto.set_model_loaded(status.model_loaded);
    proto.set_ready(status.ready);
    return proto;
}

}  // namespace llama_grpc
