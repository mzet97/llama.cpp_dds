# Bridge nativo v4 (data-centric) do `llama.cpp_dds`

Implementa o bridge DDS data-centric do v4 do DDS-LLM-Orchestrator
diretamente em C++. Diferente do bridge v3 (em `../dds_bridge.{h,cpp}`),
que usa pares de topicos request/response com correlacao por UUID, o
bridge v4 fala direto com o espaco de dados canonico do orquestrador:
quatro topicos (Tasks, AgentRegistry, TaskOutput, SystemMetrics) com
chaves explicitas e politicas de QoS por topico.

## Estrutura

```
v4/
  idl/OrchestratorV4.idl      Tipos IDL canonicos (4 topicos)
  dds_v4_bridge.h             Interface C++ do bridge
  dds_v4_bridge.cpp           Implementacao via CycloneDDS C-API
  CMakeLists.txt.snippet      Snippet para incluir no build do llama.cpp_dds
  README.md                   Este arquivo
```

## Por que existe

Hoje o `LlamaCppDDSEngine` em Python (em
`dds_orchestrator_v4/agent/llama_dds_engine.py`) serve como ponte
entre o espaco data-centric do orquestrador e os topicos internos do
`llama.cpp_dds` v3. O bridge v4 elimina essa indirecao: o `llama-server`
fala direto com o orquestrador via os mesmos quatro topicos canonicos
do espaco de dados, sem agente Python intermediario.

Vantagens em relacao a versao v3 + Python bridge:

- elimina um salto de processo (agente Python desaparece)
- elimina serializacao redundante (uma unica passagem por CDR)
- aproveita politicas de QoS nativas do middleware
- o llama-server pode atuar como agente do espaco de dados sem
  reimplementar a logica de TaskConsumer

## Como integrar com o `llama-server`

1. Compilar com a flag `-DLLAMA_DDS_V4=ON` (precisa de `idlc` e
   `CycloneDDS::ddsc` encontraveis pelo CMake).

2. No codigo do servidor, durante a inicializacao:

   ```cpp
   #include "dds_v4_bridge.h"
   using namespace llama_dds_v4;

   BridgeConfig cfg;
   cfg.domain_id      = 0;
   cfg.agent_id       = "agent-server-001";
   cfg.hostname       = "host-llama-1";
   cfg.model          = "qwen3.5-0.8b";
   cfg.specialization = "TEXT";
   cfg.slots_total    = 4;

   V4Bridge bridge(cfg);
   bridge.set_task_handler([&](const TaskInfo & t) {
       // 1) bridge ja chamou mark_task_running internamente
       auto t_start = now();
       try {
           uint32_t seq = 0;
           // 2) Loop de inferencia (codigo existente do llama-server)
           for (auto token : run_inference(t.messages_json,
                                            t.temperature,
                                            t.max_tokens)) {
               bool last = is_last_token(token);
               bridge.publish_token(t.task_id, seq++, token.text, last);
               if (last) break;
           }
           bridge.complete_task(t.task_id, "completion");
           bridge.on_task_completed(elapsed_ms(t_start));
       } catch (const std::exception & e) {
           bridge.fail_task(t.task_id, e.what());
       }
   });

   bridge.init();
   bridge.start();
   // ... loop principal do servidor mantem-se ...
   bridge.stop();
   ```

3. Ativar no servidor com `--mode v4` (ou variavel de ambiente
   `LLAMA_DDS_V4=1`). O bridge v3 (request/response) pode coexistir,
   permitindo migracao gradual.

## Limitacoes desta versao

- O `update_task_status` simplifica e republica apenas alguns campos
  da Task. Em producao, deve-se ler a Task atual via `dds_take_instance`
  antes para preservar os campos que nao sao alterados.
- Filtro de conteudo e aplicado em software (no callback do reader).
  Em CycloneDDS C, o `dds_create_reader` em conjunto com
  `dds_create_topic` com filtro requer extensao especifica; ficou
  como otimizacao futura.
- O bridge nao monitora ServerStatus do v3; e independente dele.

## Quando preferir esse bridge ou o `LlamaCppDDSEngine` em Python

Use **o bridge nativo v4 (este)**:
- ambiente de producao com prioridade absoluta de latencia
- multiplos `llama-server` no mesmo dominio com agentes em C++
- experimento E1 medindo latencia de transporte sem overhead Python

Use **`LlamaCppDDSEngine` (Python)**:
- iteracao rapida durante desenvolvimento
- reuso da logica de TaskConsumer em Python (retry, EMA, especializacao)
- compatibilidade com `llama.cpp_dds` ja instalado sem
  recompilar o servidor
