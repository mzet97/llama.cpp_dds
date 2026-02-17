# Plano de Implementação CycloneDDS - llama.cpp DDS

## Visão Geral

Este documento detalha o plano para implementar suporte completo a DDS (Data Distribution Service) usando CycloneDDS no llama.cpp, permitindo comunicação 100% DDS além do HTTP existente.

---

## Estrutura Atual (Stub)

```
dds/
├── README.md           # Documentação
├── CMakeLists.txt     # Build config
├── dds_types.h       # Tipos C++ (ChatCompletionRequest, Response, etc)
├── dds_transport.h   # Interface abstrata
├── dds_transport.cpp # Implementação (stub)
├── dds_bridge.h     # Ponte DDS ↔ server
└── dds_bridge.cpp   # Implementação (stub)
```

---

## Fase 1: Tipos DDS e IDL

### 1.1 Criar Arquivo IDL

Criar `dds/idl/LlamaDDS.idl` com as definições de tipos:

```idl
module llama {

    struct ChatMessage {
        string role;
        string content;
    };

    struct ChatCompletionRequest {
        @key string request_id;
        string model;
        sequence<ChatMessage> messages;
        float temperature;
        int32_t max_tokens;
        boolean stream;
        sequence<float> top_p;      // Optional
        sequence<int32_t> n;        // Optional
        sequence<string> stop;      // Optional
    };

    struct ChatCompletionResponse {
        @key string request_id;
        string model;
        string content;
        string finish_reason;  // "stop", "length", null
        boolean is_final;
        int32_t prompt_tokens;
        int32_t completion_tokens;
    };

    struct ServerStatus {
        @key string server_id;
        int32_t slots_idle;
        int32_t slots_processing;
        string model_loaded;
        boolean ready;
    };
};
```

### 1.2 Gerar Tipos com idlc

```bash
cd dds/idl
idlc -l ddsLlama LlamaDDS.idl
```

Isso gera:
- `LlamaDDS.h`
- `LlamaDDS.cpp`

### 1.3 Atualizar dds_types.h

Refatorar para usar tipos gerados do IDL ou manter wrappers compatíveis.

---

## Fase 2: Transporte DDS Completo

### 2.1 Implementar Participant, Topics, DataWriters/DataReaders

```cpp
// Pseudocode - dds_transport.cpp

class DDSTransportImpl {
    dds::DomainParticipant participant_;
    dds::Topic<Request> request_topic_;
    dds::Topic<Response> response_topic_;
    dds::Topic<Status> status_topic_;

    dds::DataReader<Request> request_reader_;
    dds::DataWriter<Response> response_writer_;
    dds::DataWriter<Status> status_writer_;

    // ...
};
```

### 2.2 Configuração de QoS

| Tópico | QoS | Justificativa |
|--------|-----|---------------|
| Request | RELIABLE + DURABLE | Garante entrega |
| Response | RELIABLE | Garante resposta |
| Status | BEST_EFFORT + TRANSIENT_LOCAL | Status não crítico |

### 2.3 Implementar Request-Response sobre Pub/Sub

DDS é pub/sub, não request/response. Precisamos implementar correlação:

```cpp
// Cliente envia request com request_id
// Servidor responde no tópico de response com mesmo request_id
// Cliente filtra responses por request_id
```

---

## Fase 3: Integração com Server Queue

### 3.1 Expor server_response_reader

Modificar `server_context.h` para expor o `server_response_reader`:

```cpp
struct server_context {
    // ... existente

    // Adicionar acesso ao reader para DDS
    server_response_reader& get_response_reader() {
        return queue_results_;
    }

    server_queue& get_queue() {
        return queue_tasks_;
    }
};
```

### 3.2 Converter Request DDS → server_task

No `dds_bridge.cpp`:

```cpp
void handle_request(const ChatCompletionRequest& req) {
    // 1. Converter para JSON (já existe)
    json body = dds_request_to_json(req);

    // 2. Criar server_task
    server_task task(SERVER_TASK_TYPE_COMPLETION);
    task.tokens = /* tokenizar prompt */;
    task.params = server_task::params_from_json(...);

    // 3. Obter task ID
    int task_id = response_reader_.get_new_id();
    task.id = task_id;

    // 4. Postar na queue
    response_reader_.post_task(std::move(task));

    // 5. Aguardar resultado
    auto result = response_reader_.next(/* should_stop */);

    // 6. Converter para DDS response e enviar
    send_dds_response(req.request_id, result);
}
```

### 3.3 Streaming Support

Para streaming, o servidor gera múltiplos chunks:

```cpp
// Cada token/trompt gerado = uma DDS sample
// Cliente subscribe e累积 chunks
```

---

## Fase 4: Cliente DDS de Teste

### 4.1 Criar `examples/dds-client/`

```
examples/dds-client/
├── CMakeLists.txt
└── main.cpp
```

Funcionalidades:
- Conectar ao DDS domain
- Publicar ChatCompletionRequest
- Receber e imprimir ChatCompletionResponse
- Suporte a streaming

### 4.2 API Python (opcional)

```python
# exemplos/dds_client.py
from llama_dds import DDSClient

client = DDSClient(domain_id=0)
response = client.chat_completion(
    model="llama3",
    messages=[{"role": "user", "content": "Hello!"}]
)
print(response.content)
```

---

## Fase 5: Build e Distribuição

### 5.1 Atualizar CMake

```cmake
# dds/CMakeLists.txt
idlc_generate(TARGET llama_dds_types FILES idl/LlamaDDS.idl)

add_library(llama-dds
    dds_transport.cpp
    dds_bridge.cpp
    ${GENERATED_SOURCES}
)

target_link_libraries(llama-dds
    PRIVATE
        CycloneDDS-CXX::ddscxx
        llama_dds_types
        common
)
```

### 5.2 Instalação

```bash
# Build
cmake -B build -DLLAMA_DDS=ON
cmake --build build

# Instalação
make install
```

---

## Cronograma Estimado

| Fase | Descrição | Complexidade | Tempo Est. |
|------|-----------|-------------|------------|
| 1 | Tipos IDL | Baixa | 1-2 dias |
| 2 | Transporte DDS | Alta | 3-5 dias |
| 3 | Integração Server | Alta | 2-3 dias |
| 4 | Cliente Teste | Média | 1-2 dias |
| 5 | Build/Distro | Baixa | 0.5 dia |

**Total estimado: 8-13 dias**

---

## Riscos e Mitigações

| Risco | Mitigação |
|-------|-----------|
| API CycloneDDS C++ complexa | Usar exemplos da documentação |
| Request-Response sobre Pub/Sub | Implementar correlação por ID |
| Thread safety | Usar mutexes adequados |
| Performance | Benchmarking contínuo |

---

## Referências

- [CycloneDDS Documentation](https://docs.cyclonedds.io/)
- [CycloneDDS C++ API](https://cyclonedds.io/docs/latest/cxx/)
- [OpenDDS Comparison](https://www.opendds.org/)
- IDLgen: https://github.com/eclipse-cyclonedds/cyclonedds/tree/master/src/tools/idlc

---

## Comandos Úteis

```bash
# Testar comunicação local
# Terminal 1: Servidor
./build/bin/llama-server -m model.gguf --enable-dds --dds-domain 0

# Terminal 2: Cliente
./build/bin/dds-client --domain 0 --model llama3 --message "Hello"

# Monitorar tráfego DDS
# Instalar cyclonedds-tools (se disponível)
```

---

*Documento gerado em 2026-02-16*
