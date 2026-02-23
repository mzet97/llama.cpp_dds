# Plano de Implementação - DDS-LLM-Orquestrator

## Visão Geral

Sistema completo de orquestração de agentes LLM com DDS, composto por 3 projetos separados integrados via DDS.

## Estrutura de Projetos (Repositórios Separados)

```
tese/
├── llama.cpp_dds/           # Servidor LLM (C++ com DDS)
│   ├── dds/                 # Implementação DDS
│   │   ├── dds_bridge.h/cpp # Bridge servidor <-> DDS
│   │   ├── dds_transport.h/cpp # Camada baixa DDS
│   │   └── idl/             # Definições IDL
│   │       ├── LlamaDDS.idl      # Tipos para LLM
│   │       └── OrchestratorDDS.idl # Tipos para orquestração
│   └── build/                # Binários compilados
│
├── orchestrator/             # Orquestrador Python
│   ├── main.py              # FastAPI entry point
│   ├── config.py            # Configurações
│   ├── dds.py               # Camada DDS (parcial)
│   ├── dds_client.py        # Cliente DDS completo
│   ├── registry.py          # Registro de agentes
│   ├── scheduler.py         # Scheduler de tarefas
│   ├── server.py            # Servidor HTTP
│   ├── selector.py          # Seletor de agentes
│   ├── api/
│   │   └── routes.py        # Rotas REST
│   └── models.py            # Modelos Pydantic
│
└── agent/                   # Agente Python
    ├── proxy.py             # AgentProxy (retry, contexto, fila)
    └── python/
        └── agent_llm.py     # Wrapper llama-server
```

## Integração via DDS

```
                    ┌─────────────────┐
                    │    Cliente      │
                    └────────┬────────┘
                             │ HTTP/DDS
                             ▼
┌─────────────────────────────────────────────────────────┐
│                   ORCHESTRATOR                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐     │
│  │ Registry │  │ Scheduler │  │  DDS Client     │     │
│  └────┬─────┘  └────┬─────┘  └────────┬─────────┘     │
└───────┼──────────────┼──────────────────┼──────────────┘
        │              │                  │ DDS
        │              │                  ▼
        │              │         ┌────────────────┐
        │              │         │  Tópicos DDS   │
        │              │         │ agent/register │
        │              │         │ agent/request │
        │              │         │ agent/response│
        │              │         │ agent/status  │
        │              │         └────────┬───────┘
        │              │                  │
        │              │                  ▼ DDS
        │              │         ┌────────────────┐
        └──────────────┼────────►│     AGENT     │
                       │         │  ┌─────────┐  │
                       │         │  │  LLM    │  │
                       │         │  │ Engine  │  │
                       │         │  └────┬────┘  │
                       │         └────────┼─────┘
                       │                  │
                       │                  ▼ HTTP/DDS
                       │         ┌────────────────┐
                       │         │ llama.cpp_dds  │
                       │         │   (LLM Server) │
                       │         └────────────────┘
```

### DDS Topics

| Tópico | Tipo | Descrição | QoS |
|--------|------|-----------|-----|
| agent/register | Pub/Sub | Registro de agentes | Reliable |
| agent/request | Pub/Sub | Requisições de tarefas | Reliable |
| agent/response | Pub/Sub | Respostas dos agentes | Reliable |
| agent/status | Pub/Sub | Status heartbeat | BestEffort |

## Fases de Implementação

### FASE 1: Fundamentos - ✅ CONCLUÍDO

- [x] Estrutura base do Orchestrator
- [x] HTTP Client fallback
- [x] Modelos Pydantic
- [x] DDS Client (em dds_client.py - completo)
- [x] Camada DDS em dds.py - **IMPLEMENTADO**
  - [x] `_create_topics()` - Cria tópicos com tipos IDL dinâmicos
  - [x] `_create_pubsub()` - Cria DataWriters e DataReaders
  - [x] `publish()` - Publica mensagens nos tópicos
  - [x] `read_messages()` - Lê mensagens dos tópicos
  - [x] `read_status_updates()` - Lê status dos agentes
  - [x] `read_responses()` - Lê respostas das tarefas
  - [x] `close()` - Fecha conexões DDS

### FASE 2: Agente - ✅ CONCLUÍDO

- [x] Agent Registry com heartbeat
- [x] Task Scheduler (FIFO/Priority)
- [x] AgentProxy com retry e contexto

### FASE 3: API REST - ✅ CONCLUÍDO

- [x] FastAPI Server
- [x] Rotas REST
- [x] WebSocket para streaming - **IMPLEMENTADO**
  - [x] `/v1/chat/completions` (WebSocket)
  - [x] `/v1/chat/completions_stream` (SSE)

### FASE 4: Integração e Testes - ✅ CONCLUÍDO

- [x] Conexão Orchestrator ↔ Agents (HTTP fallback)
- [x] Testes unitários (em test_orchestrator.py)
- [x] **Testes de integração DDS** (em test_dds.py)
  - [x] Teste de inicialização DDS
  - [x] Teste de latência de publish/subscribe
  - [x] Teste de throughput
- [x] **Teste DDS round-trip** (em test_dds_roundtrip.py)
  - [x] Same-participant loopback
  - [x] Two-participant round-trip
  - [x] Latência publish+read
- [x] **Teste end-to-end** (em test_e2e.py)
  - [x] Mock LLM server
  - [x] Mock agent server
  - [x] Full flow: Client → Orchestrator → Agent → Mock LLM → back
- [x] **Benchmark de overhead DDS** (em benchmark_dds_overhead.py)
  - [x] Serialização/deserialização JSON
  - [x] DDS publish latency
  - [x] HTTP overhead comparativo
- [x] **Benchmarks de latência** — executado na VM
  - [x] Servidor DDS iniciado na porta 8095
  - [x] benchmark_final DDS executado (10 rodadas)
  - [x] Dados coletados
- [x] **Correções aplicadas** (22/02/2026)
  - [x] Fix selector.py: `requires_visions` → `requires_vision`
  - [x] Fix server.py: `asyncio.StreamBuffer` → lógica direta de generate
  - [x] Fix benchmark URLs: `/api/v1/v1/` → `/v1/`
  - [x] Integração AgentSelector ↔ AgentRegistry no server.py
  - [x] Fix `_cleanup_loop` lock (instance-level em vez de local)
  - [x] Ativação DDS publish no proxy.py
  - [x] WebSocket/SSE com roteamento real para agents
  - [x] Startup assíncrono do LLMEngine
  - [x] Graceful shutdown com atexit/signals
  - [x] Fix test_orchestrator: TaskScheduler(config) e get_stats() async
  - [x] get_stats() async em registry.py e scheduler.py
- [x] Documentação

### Resultados dos Benchmarks (v2 — 23/02/2026)

> **Correção aplicada:** O poll interval em `dds_poll_loop` foi reduzido de 50ms para 5000ms
> (condition-variable-based wake), eliminando o floor artificial de ~100ms.

#### B0 — DDS Single-Client (N=10, GPU -ngl 99):
| Prompt | Mean (ms) | Std | p50 (ms) | p95 (ms) |
|--------|-----------|-----|----------|----------|
| Simple | 41.7 | 22.8 | 31.0 | 94.8 |
| Medium | 106.3 | 8.2 | 109.5 | 117.3 |
| Complex | 94.5 | 3.2 | 93.9 | 99.6 |

#### HTTP Baseline (N=10, GPU -ngl 99):
| Prompt | Mean (ms) | p50 (ms) |
|--------|-----------|----------|
| Simple | ~78 | ~78 |
| Medium | ~141 | ~141 |
| Complex | ~132 | ~132 |

**DDS vs HTTP:** B0 simple é **~2x mais rápido** que HTTP; complex é **~1.4x mais rápido**.

### Benchmark B1 - Multi-Client (v2 — 23/02/2026)

**B1: Multi-Client Benchmark** (DDS, N=10)

| Clients | Tempo Total |
|---------|-------------|
| 1 | ~1.5s |
| 2 | ~3.6s |
| 4 | ~5.8s |

### Benchmark B2 - Streaming (v2 — 23/02/2026)

**B2: Streaming Benchmark - TTFT & ITL** (DDS, N=10)

| Métrica | Simple | Complex |
|---------|--------|---------|
| TTFT (mean) | 6.3ms | 17.0ms |
| ITL (mean) | 3.2ms | 3.26ms |
| Total (mean) | 49.8ms | 340.7ms |
| Chunks | ~9-18 | 101 |

**HTTP Streaming (Complex):** ~560ms total, 103 chunks.

### Comparação Final (v2 — 23/02/2026)

| Cenário | DDS | HTTP | Razão |
|---------|-----|------|-------|
| B0 Simple (p50) | 31.0ms | ~78ms | DDS **2.5x mais rápido** |
| B0 Medium (p50) | 109.5ms | ~141ms | DDS **1.3x mais rápido** |
| B0 Complex (p50) | 93.9ms | ~132ms | DDS **1.4x mais rápido** |
| B2 Streaming TTFT | 17.0ms | — | Excelente responsividade |
| B2 Streaming ITL | 3.26ms | — | Token-a-token fluido |
| B2 Streaming Total | 340.7ms | ~560ms | DDS **1.6x mais rápido** |
| B1 Multi (4 clients) | ~5.8s total | — | Escala linear |

> Benchmarks v2 executados após correção do poll interval (50ms → cv-based wake).
> Todos os testes com GPU RX 7900 XTX, TinyLlama 1.1B Q4_K_M, 10 iterações.
- [ ] Documentação

## Como Executar

### 1. Compilar llama.cpp_dds

```bash
cd ~/llama.cpp_dds
mkdir -p build && cd build
cmake .. -DLLAMA_DDS=ON -DAMDGPU_TARGETS=gfx1032
make -j$(nproc)
```

### 2. Executar Orchestrator

```bash
cd orchestrator
pip install -r requirements.txt
python main.py --port 8080 --dds-domain 0
```

### 3. Executar Agent

```bash
cd agent/python
python agent_llm.py --model-path ../models/phi4-mini.gguf
```

### 4. Testar

```bash
curl -X POST http://localhost:8080/api/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "phi4-mini",
    "messages": [{"role": "user", "content": "Hello!"}]
  }'
```

## Endpoints API

```
GET  /api/v1/health              - Health check
POST /api/v1/agents/register     - Registrar agente
GET  /api/v1/agents             - Listar agentes
POST /api/v1/chat/completions    - Chat completions (OpenAI compatible)
GET  /api/v1/models             - Listar modelos
```

## Tipos IDL

### LlamaDDS.idl (LLM ↔ Agent)

- `ChatMessage` - Mensagem de chat
- `ChatCompletionRequest` - Requisição (OpenAI compatible)
- `ChatCompletionResponse` - Resposta
- `ServerStatus` - Status do servidor

### OrchestratorDDS.idl (Orchestrator ↔ Agent)

- `AgentRegistration` - Registro de agente
- `AgentStatus` - Status heartbeat
- `TaskRequest` - Requisição de tarefa
- `TaskResponse` - Resposta de tarefa
- `ClientRequest` - Requisição do cliente
- `ClientResponse` - Resposta ao cliente

## Métricas de Desempenho (Objetivo)

| Métrica | Target |
|---------|--------|
| Latência DDS (transporte) | < 1ms |
| Latência Orchestrator → Agent | < 5ms |
| Throughput | > 100 req/s |
| Latência LLM (inference) | Depende do modelo |

---

## Notas

- Cada projeto pode ser buildado/testado independently
- Integração via DDS permite baixo acoplamento
- Fallback HTTP quando DDS não disponível

## Decisões Arquiteturais

### Por que aiohttp (server.py) + FastAPI (routes.py) coexistem

- **`server.py` (aiohttp)** — servidor principal (canonical). Gerencia o ciclo de vida
  completo: heartbeat, cleanup, DDS pub/sub, e seleção de agentes via `AgentSelector`.
- **`api/routes.py` (FastAPI)** — API alternativa OpenAI-compatível. Fornece endpoints
  REST/WebSocket/SSE com modelos Pydantic. Não deve ser executada em paralelo com
  `server.py` em produção sem compartilhar o mesmo registry/scheduler.
- **Decisão:** `server.py` é o ponto de entrada padrão via `main.py`.
  `routes.py` serve como referência e pode ser integrada futuramente.

### Trade-offs DDS vs HTTP

| Aspecto | DDS | HTTP |
|---------|-----|------|
| Latência transporte | < 1ms (pub/sub local) | 1-5ms (TCP round-trip) |
| Complexidade | Alta (IDL, build C++) | Baixa (JSON REST) |
| Descoberta | Automática (DDS Discovery) | Manual (registro explícito) |
| Streaming | Pub/Sub contínuo | SSE / WebSocket |
| Fallback | Não disponível offline | Sempre funciona |

### Estratégia de Fallback

1. Se DDS está disponível (`dds_layer.is_available()`): usa pub/sub DDS
2. Se não: fallback automático para HTTP direto ao agent
3. O agent registra-se via REST, então o orchestrator conhece host:port para HTTP

## Known Limitations

1. **B3 (Network Delay):** `tc netem` não aplica delay no loopback para localhost;
   B3 requer máquinas separadas ou namespaces de rede.
2. **Streaming DDS:** ✅ **RESOLVIDO** — O streaming token-a-token via DDS funciona
   corretamente. O B2 confirma 101 chunks com TTFT=17ms e ITL=3.26ms. O problema
   anterior (1 chunk) era causado pelo poll interval de 50ms que foi corrigido.
3. **Dois registries:** `server.py` usa `AgentRegistry` (dataclass), `routes.py` usa
   modelos Pydantic separados. Unificação futura recomendada.
