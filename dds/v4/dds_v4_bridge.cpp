// dds_v4_bridge.cpp
// Implementacao do bridge data-centric v4.
//
// Usa a API C do CycloneDDS (mesmo padrao do dds_bridge.cpp v3).
// Os tipos sao gerados por idlc a partir de v4/idl/OrchestratorV4.idl
// (incluidos via "idl/OrchestratorV4.h" — disponibilizar no caminho de
// include do CMake).
//
// NOTA DE COMPILACAO
// ------------------
// Os simbolos `dds_llm_orchestrator_*_desc`, `dds_entity_t`, etc.
// sao definidos:
//   - pelo header `<dds/dds.h>` do CycloneDDS C-API
//   - pelo header gerado `idl/OrchestratorV4.h` (saida do `idlc`)
//
// Se o linter mostrar `Unknown type name`, o ambiente nao tem
// CycloneDDS C instalado nem rodou o `idlc` para gerar os bindings.
// O build real (CMake com `-DLLAMA_DDS_V4=ON`) resolve esses caminhos
// conforme `dds/v4/CMakeLists.txt.snippet`.

#include "dds_v4_bridge.h"

// CycloneDDS C-API completo (dds_entity_t, dds_create_*, dds_qset_*, ...).
// Tem que vir antes do header gerado, que so inclui dds_public_impl.h
// (subset que nao define dds_entity_t).
#include <dds/dds.h>

// Tipos gerados pelo idlc — caminho relativo. Espera-se que o build
// gere OrchestratorV4.h ao lado deste arquivo.
#include "idl/OrchestratorV4.h"

#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace llama_dds_v4 {

namespace {

inline uint64_t now_ns() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

inline char * dup_string(const std::string & s) {
    char * out = static_cast<char *>(std::malloc(s.size() + 1));
    if (out) std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

inline std::string from_c_str(const char * s) {
    return s ? std::string(s) : std::string();
}

inline std::string spec_to_string(int32_t code) {
    switch (code) {
        case 0:  return "TEXT";
        case 1:  return "VISION";
        case 2:  return "EMBEDDING";
        default: return "TEXT";
    }
}

} // anonymous namespace

// ============================================================
// V4BridgeImpl: encapsula entidades DDS C-API
// ============================================================

class V4BridgeImpl {
  public:
    explicit V4BridgeImpl(const BridgeConfig & cfg) : cfg_(cfg) {}

    ~V4BridgeImpl() { close(); }

    bool open() {
        participant_ = dds_create_participant(cfg_.domain_id, nullptr, nullptr);
        if (participant_ < 0) {
            std::cerr << "v4_bridge: dds_create_participant: "
                      << dds_strretcode(-participant_) << std::endl;
            return false;
        }

        // Topico Tasks
        topic_tasks_ = dds_create_topic(participant_,
                                        &dds_llm_orchestrator_Task_desc,
                                        "Tasks", nullptr, nullptr);
        if (topic_tasks_ < 0) return false;

        // Topico AgentRegistry
        topic_agents_ = dds_create_topic(participant_,
                                         &dds_llm_orchestrator_AgentState_desc,
                                         "AgentRegistry", nullptr, nullptr);
        if (topic_agents_ < 0) return false;

        // Topico TaskOutput
        topic_outputs_ = dds_create_topic(participant_,
                                          &dds_llm_orchestrator_TaskOutput_desc,
                                          "TaskOutput", nullptr, nullptr);
        if (topic_outputs_ < 0) return false;

        // Topico SystemMetrics
        topic_metrics_ = dds_create_topic(participant_,
                                          &dds_llm_orchestrator_SystemMetric_desc,
                                          "SystemMetrics", nullptr, nullptr);
        if (topic_metrics_ < 0) return false;

        // QoS por topico
        dds_qos_t * qos_tasks = dds_create_qos();
        dds_qset_reliability(qos_tasks, DDS_RELIABILITY_RELIABLE,
                             DDS_SECS(10));
        dds_qset_durability(qos_tasks, DDS_DURABILITY_TRANSIENT_LOCAL);
        dds_qset_history(qos_tasks, DDS_HISTORY_KEEP_LAST, 50);

        dds_qos_t * qos_agents = dds_create_qos();
        dds_qset_reliability(qos_agents, DDS_RELIABILITY_RELIABLE,
                             DDS_SECS(10));
        dds_qset_durability(qos_agents, DDS_DURABILITY_TRANSIENT_LOCAL);
        dds_qset_history(qos_agents, DDS_HISTORY_KEEP_LAST, 1);
        dds_qset_deadline(qos_agents, DDS_SECS(30));
        dds_qset_ownership(qos_agents, DDS_OWNERSHIP_EXCLUSIVE);

        dds_qos_t * qos_outputs = dds_create_qos();
        dds_qset_reliability(qos_outputs, DDS_RELIABILITY_RELIABLE,
                             DDS_SECS(10));
        dds_qset_durability(qos_outputs, DDS_DURABILITY_VOLATILE);
        dds_qset_history(qos_outputs, DDS_HISTORY_KEEP_LAST, 20);
        dds_qset_deadline(qos_outputs, DDS_SECS(10));
        dds_qset_ownership(qos_outputs, DDS_OWNERSHIP_EXCLUSIVE);

        dds_qos_t * qos_metrics = dds_create_qos();
        dds_qset_reliability(qos_metrics, DDS_RELIABILITY_BEST_EFFORT, 0);
        dds_qset_durability(qos_metrics, DDS_DURABILITY_VOLATILE);
        dds_qset_history(qos_metrics, DDS_HISTORY_KEEP_LAST, 1);

        // Reader de Tasks com filtro de conteudo
        // Filtro: assigned_agent == my_id AND status == 1 (ASSIGNED)
        std::string filter_expr = "assigned_agent = '" + cfg_.agent_id +
                                  "' AND status = 1";
        // Em CycloneDDS C API, content-filtered topic e criado em duas etapas;
        // como simplificacao, filtramos em software dentro do reader callback.
        reader_tasks_ = dds_create_reader(participant_, topic_tasks_,
                                          qos_tasks, nullptr);
        if (reader_tasks_ < 0) return false;

        // Writers
        writer_tasks_   = dds_create_writer(participant_, topic_tasks_,
                                            qos_tasks, nullptr);
        writer_agents_  = dds_create_writer(participant_, topic_agents_,
                                            qos_agents, nullptr);
        writer_outputs_ = dds_create_writer(participant_, topic_outputs_,
                                            qos_outputs, nullptr);
        writer_metrics_ = dds_create_writer(participant_, topic_metrics_,
                                            qos_metrics, nullptr);

        dds_delete_qos(qos_tasks);
        dds_delete_qos(qos_agents);
        dds_delete_qos(qos_outputs);
        dds_delete_qos(qos_metrics);

        return writer_tasks_ >= 0 && writer_agents_ >= 0
            && writer_outputs_ >= 0 && writer_metrics_ >= 0;
    }

    void close() {
        if (participant_ >= 0) {
            dds_delete(participant_);
            participant_ = -1;
        }
    }

    /// Le tasks pendentes (filtrando por agent_id em software).
    /// Retorna numero de tasks adicionadas a out.
    int read_my_tasks(std::vector<TaskInfo> & out, const std::string & my_id) {
        constexpr unsigned MAX = 32;
        dds_llm_orchestrator_Task * samples[MAX];
        dds_sample_info_t            infos[MAX];
        for (unsigned i = 0; i < MAX; ++i) samples[i] = nullptr;

        int n = dds_take(reader_tasks_, reinterpret_cast<void **>(samples),
                         infos, MAX, MAX);
        if (n <= 0) return 0;

        int added = 0;
        for (int i = 0; i < n; ++i) {
            if (!infos[i].valid_data || samples[i] == nullptr) continue;
            const auto & t = *samples[i];
            if (from_c_str(t.assigned_agent) != my_id) continue;
            if (t.status != 1) continue;  // 1 = ASSIGNED

            TaskInfo info;
            info.task_id        = from_c_str(t.task_id);
            info.client_id      = from_c_str(t.client_id);
            info.model_required = spec_to_string(t.model_required);
            info.model_name     = from_c_str(t.model_name);
            info.messages_json  = from_c_str(t.messages_json);
            info.temperature    = t.temperature;
            info.max_tokens     = t.max_tokens;
            info.stream         = t.stream;
            info.priority       = t.priority;
            info.created_at_ns  = t.created_at_ns;
            info.deadline_ns    = t.deadline_ns;
            info.retry_count    = t.retry_count;
            out.push_back(std::move(info));
            ++added;
        }
        dds_return_loan(reader_tasks_,
                        reinterpret_cast<void **>(samples), n);
        return added;
    }

    /// Publica AgentState com os campos atuais.
    void publish_agent_state(const BridgeConfig & cfg, AgentHealth health,
                              uint32_t slots_busy, float ema_lat,
                              uint32_t completed, uint32_t failed,
                              uint64_t uptime_s) {
        dds_llm_orchestrator_AgentState state{};
        state.agent_id        = dup_string(cfg.agent_id);
        state.hostname        = dup_string(cfg.hostname);
        state.model           = dup_string(cfg.model);
        state.specialization  = dup_string(cfg.specialization);
        state.slots_total     = cfg.slots_total;
        state.slots_busy      = slots_busy;
        state.vram_total_mb   = cfg.vram_total_mb;
        state.vram_used_mb    = 0;
        state.ema_latency_ms  = ema_lat;
        state.completed_total = completed;
        state.failed_total    = failed;
        state.health          = static_cast<int32_t>(health);
        state.last_update_ns  = now_ns();
        state.uptime_seconds  = uptime_s;
        dds_write(writer_agents_, &state);

        std::free(state.agent_id);
        std::free(state.hostname);
        std::free(state.model);
        std::free(state.specialization);
    }

    /// Publica TaskOutput.
    void publish_output(const std::string & task_id, uint32_t seq,
                         const std::string & content, bool is_final,
                         FinishReason fr) {
        dds_llm_orchestrator_TaskOutput out{};
        out.task_id       = dup_string(task_id);
        out.seq_num       = seq;  // 'sequence' e palavra reservada em IDL
        out.content       = dup_string(content);
        out.is_final      = is_final;
        out.finish_reason = static_cast<int32_t>(fr);
        out.emitted_at_ns = now_ns();
        dds_write(writer_outputs_, &out);
        std::free(out.task_id);
        std::free(out.content);
    }

    /// Publica SystemMetric.
    void publish_metric(const std::string & name, const std::string & component_id,
                        int32_t component_type, double value,
                        const std::string & unit) {
        dds_llm_orchestrator_SystemMetric m{};
        m.metric_name    = dup_string(name);
        m.component_id   = dup_string(component_id);
        m.component_type = component_type;
        m.value          = value;
        m.unit           = dup_string(unit);
        m.timestamp_ns   = now_ns();
        dds_write(writer_metrics_, &m);
        std::free(m.metric_name);
        std::free(m.component_id);
        std::free(m.unit);
    }

    /// Atualiza status de uma tarefa republicando-a no Tasks.
    /// O orquestrador subscreve Tasks e ve a atualizacao.
    /// Esta versao reusa as informacoes ja conhecidas do TaskInfo;
    /// em uma implementacao completa, leriamos a Task atual primeiro.
    void update_task_status(const std::string & task_id,
                             const std::string & assigned_agent,
                             TaskStatus new_status,
                             const std::string & finish_reason) {
        // Constroi instancia minimal — caller deve preencher campos
        // estaveis. Em producao real e melhor ler primeiro a Task atual
        // e modificar apenas os campos relevantes; aqui simplificamos.
        dds_llm_orchestrator_Task t{};
        t.task_id        = dup_string(task_id);
        t.assigned_agent = dup_string(assigned_agent);
        t.status         = static_cast<int32_t>(new_status);
        t.finish_reason  = dup_string(finish_reason);
        if (new_status == TaskStatus::RUNNING) {
            t.started_at_ns = now_ns();
        } else if (new_status == TaskStatus::DONE
                || new_status == TaskStatus::FAILED) {
            t.completed_at_ns = now_ns();
        }
        // Os campos nao preenchidos ficam zero/nulos. Em producao,
        // implementar dds_take_instance() antes para preservar dados.
        dds_write(writer_tasks_, &t);
        std::free(t.task_id);
        std::free(t.assigned_agent);
        std::free(t.finish_reason);
    }

  private:
    BridgeConfig cfg_;
    dds_entity_t participant_   = -1;
    dds_entity_t topic_tasks_   = -1;
    dds_entity_t topic_agents_  = -1;
    dds_entity_t topic_outputs_ = -1;
    dds_entity_t topic_metrics_ = -1;
    dds_entity_t reader_tasks_  = -1;
    dds_entity_t writer_tasks_  = -1;
    dds_entity_t writer_agents_ = -1;
    dds_entity_t writer_outputs_ = -1;
    dds_entity_t writer_metrics_ = -1;
};

// ============================================================
// V4Bridge — interface publica
// ============================================================

V4Bridge::V4Bridge(const BridgeConfig & config)
    : config_(config),
      start_time_(std::chrono::steady_clock::now()),
      pimpl_(std::make_unique<V4BridgeImpl>(config)) {
}

V4Bridge::~V4Bridge() { stop(); }

bool V4Bridge::init() {
    if (initialized_.load()) return true;
    if (config_.agent_id.empty()) return false;
    bool ok = pimpl_->open();
    initialized_.store(ok);
    return ok;
}

void V4Bridge::set_task_handler(TaskHandler handler) {
    task_handler_ = std::move(handler);
}

bool V4Bridge::start() {
    if (!initialized_.load()) {
        if (!init()) return false;
    }
    running_.store(true);

    // Thread reader: le Tasks atribuidas a este agente
    std::thread([&]() {
        std::vector<TaskInfo> batch;
        while (running_.load()) {
            batch.clear();
            int n = pimpl_->read_my_tasks(batch, config_.agent_id);
            for (auto & t : batch) {
                if (task_handler_) {
                    slots_busy_.fetch_add(1);
                    pimpl_->update_task_status(
                        t.task_id, config_.agent_id,
                        TaskStatus::RUNNING, "");
                    task_handler_(t);
                    slots_busy_.fetch_sub(1);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            (void)n;
        }
    }).detach();

    // Thread heartbeat: publica AgentState periodicamente
    std::thread([&]() {
        while (running_.load()) {
            publish_heartbeat();
            std::this_thread::sleep_for(config_.heartbeat_interval);
        }
    }).detach();

    // Thread metrics: publica vazao agregada a cada metrics_interval
    std::thread([&]() {
        uint32_t prev_completed = 0;
        auto     prev_t = std::chrono::steady_clock::now();
        while (running_.load()) {
            std::this_thread::sleep_for(config_.metrics_interval);
            auto     now = std::chrono::steady_clock::now();
            uint32_t cur = completed_total_.load();
            double   secs = std::chrono::duration<double>(now - prev_t).count();
            if (secs > 0) {
                double rps = (cur - prev_completed) / secs;
                pimpl_->publish_metric("agent_throughput_rps",
                                        config_.agent_id, 1, rps, "req/s");
            }
            prev_completed = cur;
            prev_t = now;
        }
    }).detach();

    return true;
}

void V4Bridge::stop() {
    running_.store(false);
    // Threads detachadas vao terminar quando running_ for false
    initialized_.store(false);
}

void V4Bridge::publish_token(const std::string & task_id, uint32_t sequence,
                             const std::string & content, bool is_final,
                             FinishReason finish_reason) {
    if (!initialized_.load()) return;
    pimpl_->publish_output(task_id, sequence, content, is_final, finish_reason);
}

void V4Bridge::mark_task_running(const std::string & task_id) {
    pimpl_->update_task_status(task_id, config_.agent_id,
                                TaskStatus::RUNNING, "");
}

void V4Bridge::complete_task(const std::string & task_id,
                             const std::string & finish_reason) {
    pimpl_->update_task_status(task_id, config_.agent_id,
                                TaskStatus::DONE, finish_reason);
}

void V4Bridge::fail_task(const std::string & task_id,
                          const std::string & reason) {
    pimpl_->update_task_status(task_id, config_.agent_id,
                                TaskStatus::FAILED, reason);
    failed_total_.fetch_add(1);
}

void V4Bridge::on_task_completed(double latency_ms) {
    completed_total_.fetch_add(1);
    // EMA com alpha = 0.3
    float prev = ema_latency_ms_.load();
    float next = (prev == 0.0f) ? static_cast<float>(latency_ms)
                                : 0.3f * static_cast<float>(latency_ms)
                                  + 0.7f * prev;
    ema_latency_ms_.store(next);
}

void V4Bridge::on_task_failed() { failed_total_.fetch_add(1); }

void V4Bridge::publish_heartbeat() {
    if (!initialized_.load()) return;
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time_).count();
    pimpl_->publish_agent_state(
        config_, health_.load(),
        slots_busy_.load(), ema_latency_ms_.load(),
        completed_total_.load(), failed_total_.load(),
        static_cast<uint64_t>(uptime));
}

void V4Bridge::set_health(AgentHealth h) { health_.store(h); }

} // namespace llama_dds_v4
