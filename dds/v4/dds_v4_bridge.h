// dds_v4_bridge.h
// Bridge data-centric do llama.cpp para o espaco de dados v4 do
// DDS-LLM-Orchestrator.
//
// Substitui o modelo request/response correlacionado por UUID
// (dds_bridge.h) por DCPS canonico:
//
//   - Subscreve `Tasks` com filtro de conteudo:
//         assigned_agent = '<my_agent_id>' AND status = 1 (ASSIGNED)
//   - Para cada Task recebida:
//         atualiza status -> RUNNING
//         executa inferencia
//         publica tokens em `TaskOutput` com chave (task_id, sequence)
//         atualiza status -> DONE
//   - Publica AgentState periodicamente em `AgentRegistry`
//   - Publica metricas em `SystemMetrics` (vazao, latencia EMA, slots)
//
// Esse bridge elimina o agente Python como intermediario: o llama-server
// fala direto com o orquestrador via espaco de dados DDS, sem indirecao
// HTTP nem topicos auxiliares.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace llama_dds_v4 {

/// Estado funcional do agente (espelha AgentHealth do IDL).
enum class AgentHealth : int32_t {
    OFFLINE  = 0,
    DEGRADED = 1,
    HEALTHY  = 2
};

/// Razao de termino de uma tarefa.
enum class FinishReason : int32_t {
    NONE       = 0,
    COMPLETION = 1,
    LENGTH     = 2,
    TIMEOUT    = 3,
    ERROR_     = 4   // ERROR e macro do Windows
};

/// Status de uma tarefa no ciclo de vida.
enum class TaskStatus : int32_t {
    PENDING  = 0,
    ASSIGNED = 1,
    RUNNING  = 2,
    DONE     = 3,
    FAILED   = 4
};

/// Configuracao do bridge v4.
struct BridgeConfig {
    int         domain_id          = 0;
    std::string agent_id;                       // obrigatorio
    std::string hostname           = "localhost";
    std::string model              = "qwen3.5-0.8b";
    std::string specialization     = "TEXT";    // TEXT | VISION | EMBEDDING
    uint32_t    slots_total        = 1;
    uint32_t    vram_total_mb      = 0;
    std::chrono::milliseconds heartbeat_interval{ 5000 };
    std::chrono::milliseconds metrics_interval{ 10000 };
};

/// Estrutura de uma tarefa recebida.
/// Forma livre, sem dependencia direta dos tipos IDL para que callers
/// possam usar o bridge sem incluir os headers gerados.
struct TaskInfo {
    std::string task_id;
    std::string client_id;
    std::string model_required;     // "TEXT" | "VISION" | "EMBEDDING"
    std::string model_name;
    std::string messages_json;
    float       temperature  = 0.7f;
    uint32_t    max_tokens   = 512;
    bool        stream       = true;
    int32_t     priority     = 5;   // LOW=1, NORMAL=5, HIGH=10
    uint64_t    created_at_ns  = 0;
    uint64_t    deadline_ns    = 0;
    uint32_t    retry_count    = 0;
};

/// Callback chamado quando uma tarefa atribuida a este agente chega.
/// O caller deve produzir tokens chamando publish_token() e finalizar
/// chamando complete_task() ou fail_task().
using TaskHandler = std::function<void(const TaskInfo &)>;

// Forward decl
class V4BridgeImpl;

/// Bridge data-centric v4. Lifecycle:
///
///   V4Bridge bridge(config);
///   bridge.set_task_handler([](const TaskInfo & t) { ... });
///   bridge.start();
///   // ... agente recebe TaskInfo via callback, processa,
///   // chama bridge.publish_token(...), bridge.complete_task(...).
///   bridge.stop();
class V4Bridge {
  public:
    explicit V4Bridge(const BridgeConfig & config);
    ~V4Bridge();

    V4Bridge(const V4Bridge &)             = delete;
    V4Bridge & operator=(const V4Bridge &) = delete;

    /// Cria entidades DDS, configura QoS e subscreve Tasks com filtro.
    /// Retorna false em caso de erro de inicializacao.
    bool init();

    /// Registra o callback que processa cada tarefa recebida.
    /// Deve ser chamado antes de start().
    void set_task_handler(TaskHandler handler);

    /// Inicia threads internas (reader, heartbeat, metrics).
    bool start();

    /// Encerra threads e libera entidades DDS.
    void stop();

    /// Publica um token gerado durante a inferencia.
    /// Marca is_final=true no ultimo token.
    void publish_token(const std::string & task_id,
                       uint32_t            sequence,
                       const std::string & content,
                       bool                is_final,
                       FinishReason        finish_reason = FinishReason::NONE);

    /// Atualiza Task no espaco com status=RUNNING.
    /// Chamar ao iniciar a inferencia.
    void mark_task_running(const std::string & task_id);

    /// Atualiza Task no espaco com status=DONE e finish_reason.
    /// Chamar quando o ultimo token e publicado.
    void complete_task(const std::string & task_id,
                       const std::string & finish_reason = "completion");

    /// Atualiza Task no espaco com status=FAILED.
    /// Use quando inferencia falha por motivo nao recuperavel.
    void fail_task(const std::string & task_id, const std::string & reason);

    /// Atualiza estatisticas locais usadas pelo heartbeat.
    void on_task_completed(double latency_ms);
    void on_task_failed();

    /// Forca publicacao de heartbeat agora (alem do periodico).
    void publish_heartbeat();

    /// Agente saudavel? (lido pelo heartbeat publicador)
    void set_health(AgentHealth h);

    bool is_running() const { return running_.load(); }

  private:
    BridgeConfig                   config_;
    std::atomic<bool>              running_{ false };
    std::atomic<bool>              initialized_{ false };
    std::atomic<AgentHealth>       health_{ AgentHealth::HEALTHY };

    // Estatisticas para heartbeat
    std::atomic<uint32_t>          slots_busy_{ 0 };
    std::atomic<uint32_t>          completed_total_{ 0 };
    std::atomic<uint32_t>          failed_total_{ 0 };
    std::atomic<float>             ema_latency_ms_{ 0.0f };
    std::chrono::steady_clock::time_point start_time_;

    TaskHandler                    task_handler_;
    std::unique_ptr<V4BridgeImpl>  pimpl_;
};

} // namespace llama_dds_v4
