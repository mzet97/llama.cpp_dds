# DDS Transport Integration for llama.cpp

This document describes the integration of CycloneDDS (Data Distribution Service) into llama.cpp to enable low-latency, high-performance communication for chat completion requests.

## Overview

The DDS transport layer provides an alternative to HTTP for communicating with the llama.cpp server. DDS offers several advantages:

- **Lower latency** through optimized shared memory transport
- **Publish-subscribe pattern** for efficient many-to-many communication
- **Quality of Service (QoS)** settings for reliability and durability
- **Built-in discovery** - no need for explicit connection management

## Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  DDS Client     │────▶│  llama.cpp DDS  │────▶│   Inference     │
│  (test_client)  │     │     Server      │     │    Engine       │
└─────────────────┘     └──────────────────┘     └─────────────────┘
                               │
                               ▼
                        ┌──────────────────┐
                        │  Response via    │
                        │     DDS          │
                        └──────────────────┘
```

### Components

| Component | Description |
|-----------|-------------|
| `dds/dds_transport.cpp` | Server-side DDS transport implementation |
| `dds/idl/LlamaDDS.idl` | Interface Definition Language for chat completion types |
| `dds/test_client.cpp` | Simple C++ test client for debugging |
| `dds/benchmark_final.cpp` | Performance benchmark client |
| `benchmark_thesis.py` | Python script for HTTP vs DDS comparison |

## IDL Definition

The IDL defines the message types for DDS communication:

### ChatCompletionRequest

```idl
struct ChatCompletionRequest {
    string request_id;        // Unique request identifier (UUID)
    string model;             // Model name (e.g., "phi4-mini")
    float temperature;        // Sampling temperature (0.0-2.0)
    long max_tokens;          // Maximum tokens to generate
    boolean stream;            // Enable streaming responses
    sequence<ChatMessage> messages;  // Chat messages
};

struct ChatMessage {
    string role;              // "system", "user", "assistant"
    string content;           // Message content
};
```

### ChatCompletionResponse

```idl
struct ChatCompletionResponse {
    string request_id;        // Correlates with request
    string model;             // Model used
    string content;           // Generated content
    string finish_reason;     // "stop", "length", etc.
    boolean is_final;         // True if this is the final response
};
```

## Topics

| Topic Name | Direction | Description |
|------------|-----------|-------------|
| `llama_chat_completion_request` | Client → Server | Chat completion requests |
| `llama_chat_completion_response` | Server → Client | Chat completion responses |

## QoS Configuration

The following Quality of Service settings are used:

```cpp
dds_qos_t* qos = dds_create_qos();
dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);
```

| QoS Policy | Value | Description |
|------------|-------|-------------|
| RELIABILITY | RELIABLE, 10s timeout | Ensures messages are delivered |
| DURABILITY | TRANSIENT_LOCAL | Messages available to late-joining readers |
| HISTORY | KEEP_LAST, 8 | Keep last 8 samples for each instance |

## Building

### Prerequisites

1. **CycloneDDS** installed and built
2. **IDLC compiler** (idlc) for generating C++ from IDL

### Build Steps

```bash
# 1. Build CycloneDDS (if not already built)
cd /path/to/cyclonedds
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# 2. Generate IDL code
cd /path/to/llama.cpp_dds/dds/idl
idlc -l c++ -o ../ LlamaDDS.idl

# 3. Build the server (with DDS support)
cd /path/to/llama.cpp_dds
mkdir build && cd build
cmake .. -DLLAMA_DDS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# 4. Build test client
cd /path/to/llama.cpp_dds/dds
g++ -std=c++17 -I/path/to/cyclonedds/install/include \
    test_client.cpp idl/LlamaDDS.o \
    -o test_client \
    -L/path/to/cyclonedds/install/lib \
    -lddsc -lddscxx -pthread \
    -Wl,-rpath,/path/to/cyclonedds/install/lib
```

## Running

### Start Server with DDS

```bash
# With DDS enabled
./build/bin/llama-server \
    --enable-dds \
    --model models/phi4-mini-q4_k_m.gguf \
    --port 8080

# Or use background execution (Linux/macOS)
./build/bin/llama-server \
    --enable-dds \
    --model models/phi4-mini-q4_k_m.gguf \
    --port 8080 &
```

### Test Client

```bash
# Basic test
cd /path/to/llama.cpp_dds/dds
./test_client 0 "What is 2+2?"

# Specify domain and prompt
./test_client <domain_id> "<your prompt>"
```

### Benchmark

```bash
# Run performance benchmark
cd /path/to/llama.cpp_dds/dds
./benchmark_final 10

# Output example:
# === DDS Persistent Benchmark ===
# Tests per prompt: 10
#
# --- simple ---
# Prompt: What is 2+2?
# Mean: 6500.00 ms
# ...
```

## HTTP vs DDS Benchmark

Use the provided Python script for comprehensive HTTP benchmarking and compare with the C++ DDS benchmark:

```bash
# 1. Start server with HTTP and DDS enabled
./build/bin/llama-server \
    --enable-dds \
    --model models/phi4-mini-q4_k_m.gguf \
    --port 8080

# 2. Run DDS Benchmark (C++)
./build/bin/benchmark_final 10

# 3. Run HTTP Benchmark (Python)
python3 dds/benchmark_http.py 10
```

### Test Prompts

| Type | Prompt |
|------|--------|
| Simple | "What is 2+2?" |
| Medium | "Explain machine learning in a few sentences." |
| Complex | "Write a detailed technical explanation of how neural networks work, including backpropagation, gradient descent, and the role of activation functions." |

### Expected Results

| Metric | Description |
|--------|-------------|
| Mean | Average latency in milliseconds |
| Std | Standard deviation |
| p50 | 50th percentile latency |
| p95 | 95th percentile latency |
| p99 | 99th percentile latency |
| Throughput | Requests per second |

## Performance Optimizations

### Server-side

1.  **Event-Driven Processing**: Replaced polling loops with **DDS WaitSets**, eliminating CPU-intensive sleep loops and reducing jitter.
2.  **Optimized Read Loop**: Zero-copy sample handling where possible, with proper RAII cleanup.
3.  **QoS Configuration**: Added `RELIABILITY`, `DURABILITY`, and `HISTORY` (KeepLast 8) to prevent data loss under load.

### Client-side

1.  **Matching QoS**: Client uses same QoS as server to receive responses.
2.  **Persistent Connection**: `benchmark_final` reuses DDS entities across requests, eliminating connection setup overhead (unlike previous versions).
3.  **Proper Timeouts**: Configured appropriate wait times using WaitSets.

### Build Optimizations

```bash
# Release build with optimizations
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-Ofast -march=native" \
      ..
```

## Troubleshooting

### Client not receiving responses

**Cause**: QoS mismatch between client and server

**Solution**: Ensure client uses the same QoS settings as server:
```cpp
dds_qos_t* qos = dds_create_qos();
dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
```

### DDS entities not matching

**Cause**: Different topic names or QoS

**Solution**: Verify topic names match exactly:
- `llama_chat_completion_request`
- `llama_chat_completion_response`

### High latency

**Cause**: Default polling intervals too high

**Solution**: Reduce sleep intervals in:
- `dds/dds_transport.cpp` (read loop)
- `tools/server/server.cpp` (polling loop)

## Files Reference

| File | Purpose |
|------|---------|
| `dds/CMakeLists.txt` | CMake build configuration for DDS |
| `dds/dds_transport.cpp` | Server DDS transport implementation |
| `dds/dds_transport.h` | Transport header |
| `dds/dds_bridge.cpp` | Bridge between DDS transport and server queue |
| `dds/dds_bridge.h` | Bridge header |
| `dds/dds_idl_wrapper.h` | IDL wrapper utilities |
| `dds/dds_types.h` | C++ type definitions |
| `dds/idl/LlamaDDS.idl` | IDL type definitions |
| `dds/idl/LlamaDDS.h` | Generated C types from IDL |
| `dds/idl/LlamaDDS.c` | Generated C implementation from IDL |
| `dds/test_client.cpp` | Simple test client |
| `dds/benchmark_final.cpp` | Performance benchmark |
| `dds/benchmark_persistent.cpp` | Persistent benchmark v1 |
| `dds/benchmark_persistent_v2.cpp` | Persistent benchmark v2 |
| `dds/persistent_client.cpp` | Persistent client implementation |
| `benchmark_thesis.py` | HTTP vs DDS comparison |
| `tools/server/server.cpp` | Main server with DDS integration |
| `tools/server/server-context.cpp` | Server context with DDS bridge accessor |
| `tools/server/server-context.h` | Server context header |

---

## Implementation Analysis

### Architecture Overview

The DDS integration follows a layered architecture:

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│  test_client.cpp, benchmark_final.cpp, persistent_client    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   DDS Bridge Layer                          │
│              DDSBridge (dds_bridge.h/cpp)                  │
│  - Request queuing and tracking                            │
│  - Response sending                                        │
│  - Status publishing                                       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                 DDS Transport Layer                         │
│            DDSTransport (dds_transport.h/cpp)             │
│  - Topic management                                        │
│  - Read/Write loops                                       │
│  - QoS configuration                                      │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Server Integration                        │
│              server.cpp (process_dds_request)              │
│  - DDS polling thread                                     │
│  - Request to task conversion                            │
│  - Response extraction                                    │
└─────────────────────────────────────────────────────────────┘
```

### Key Components

#### 1. DDSTransport (dds_transport.h/cpp)

**Responsibilities:**
- DDS DomainParticipant lifecycle management
- Topic creation and management (request, response, status)
- DataReader/DataWriter creation with QoS
- Read loop for incoming requests
- Response and status publishing

**Key Features:**
- Thread-safe implementation using atomic flags
- Callback-based request handling
- Proper resource cleanup on stop

**Code Structure:**
```cpp
class DDSTransportImpl {
    // DDS entities
    dds_entity_t participant_ = 0;
    dds_entity_t request_topic_ = 0;
    dds_entity_t response_topic_ = 0;
    dds_entity_t status_topic_ = 0;
    dds_entity_t request_reader_ = 0;
    dds_entity_t response_writer_ = 0;
    dds_entity_t status_writer_ = 0;

    // Threading
    std::thread reader_thread_;
    std::atomic<bool> running_{false};

    // Callbacks
    RequestCallback request_callback_;
};
```

#### 2. DDSBridge (dds_bridge.h/cpp)

**Responsibilities:**
- Abstraction layer between transport and server
- Request queuing for thread-safe access
- Pending request tracking with mutex
- Callback registration for request processing

**Code Structure:**
```cpp
class DDSBridgeImpl {
    std::unique_ptr<DDSTransport> transport_;
    std::atomic<bool> running_{false};
    std::map<std::string, ChatCompletionRequest> pending_requests_;
    std::mutex mutex_;
    DDSBridge::ProcessRequestCallback process_callback_;
};
```

#### 3. IDL Types (idl/LlamaDDS.idl)

**Defined Types:**
- `ChatMessage` - Role and content pair
- `ChatCompletionRequest` - Request with UUID, model, messages, parameters
- `ChatCompletionResponse` - Response with content, tokens, finish reason
- `ServerStatus` - Health status for discovery
- `EmbeddingRequest/Response` - Reserved for future embedding support

### Integration with Server (server.cpp)

The server integration uses a polling pattern:

```cpp
// 1. DDS polling thread runs independently
static void dds_poll_loop(...) {
    while (running_) {
        if (dds_bridge->has_pending_requests()) {
            auto req = dds_bridge->pop_pending_request();
            process_dds_request(dds_bridge, req, ...);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// 2. Request processing converts DDS to server task
static void process_dds_request(...) {
    // Tokenize prompt
    auto tokens = tokenize(prompt);

    // Create server task
    server_task task(SERVER_TASK_TYPE_COMPLETION);
    task.tokens = server_tokens(tokens, false);
    task.params.n_predict = max_tokens;
    task.params.sampling.temp = temperature;

    // Post to queue
    queue_tasks->post(std::move(task));

    // Wait for result
    auto result = queue_results->recv_with_timeout({task.id}, 5);

    // Extract content and send DDS response
    send_response(content, tokens, finish_reason);
}
```

---

## Code Quality Assessment

### Strengths

1. **Modular Design**: Clear separation between transport, bridge, and server integration
2. **Type Safety**: IDL-generated types provide compile-time checking
3. **QoS Configuration**: Proper DDS QoS settings for reliability
4. **Resource Management**: Proper cleanup in destructors and stop methods
5. **Error Handling**: Error responses sent back to clients on failures

### Areas for Improvement

1. **Streaming Support**: Currently not fully implemented
   - `stream` flag in request is ignored
   - No partial response publishing during generation

2. **Memory Management**: Some potential memory leaks in error paths
   - `dds_string_dup` allocated strings not always freed
   - IDL wrapper conversion functions need cleanup

3. **Error Handling**: Could be more robust
   - No retry mechanism for failed DDS operations
   - Limited logging for debugging

4. **Performance**: Polling-based approach could be improved
   - Consider using DDS Waitset for event-driven notification
   - Current 1ms polling still consumes CPU

5. **Thread Safety**: Some shared state needs better protection
   - `pending_requests_` uses mutex but callback invocation is outside lock
   - Potential race condition in `has_pending_requests()` and `pop_pending_request()`

### Recommended Improvements

#### 1. Implement Waitset for Event-Driven Notification

Replace polling with DDS Waitset:

```cpp
// Current (polling)
while (running_) {
    // Check for samples
    dds_take(request_reader_, ...);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// Improved (event-driven)
dds_waitset_t ws = dds_create_waitset(participant_);
dds_waitset_attach(ws, request_reader_, 0);

while (running_) {
    dds_attach_t triggered;
    dds_waitset_wait(ws, &triggered, 1, DDS_INFINITY);
    // Process samples immediately
    dds_take(request_reader_, ...);
}
```

#### 2. Add Streaming Support

Implement partial response publishing:

```cpp
void process_dds_request(...) {
    // ... existing setup ...

    // During generation, publish partial results
    while (!is_final) {
        auto result = queue_results->recv_with_timeout(...);

        if (result->is_progress()) {
            // Send partial response
            ChatCompletionResponse partial;
            partial.request_id = req.request_id;
            partial.content = result->content;
            partial.is_final = false;
            dds_bridge->send_response(partial);
        }
    }
}
```

#### 3. Fix Memory Leaks

Add proper cleanup in wrapper conversions:

```cpp
inline void free_llama_request(llama_ChatCompletionRequest& req) {
    dds_free(req.request_id);
    dds_free(req.model);
    // Free messages array
    for (uint32_t i = 0; i < req.messages._length; i++) {
        dds_free(req.messages._buffer[i].role);
        dds_free(req.messages._buffer[i].content);
    }
    dds_free(req.messages._buffer);
    // ... cleanup other fields ...
}
```

#### 4. Improve Error Handling

Add retry logic and better error reporting:

```cpp
bool send_response_with_retry(const ChatCompletionResponse& response, int max_retries = 3) {
    for (int attempt = 0; attempt < max_retries; attempt++) {
        auto ret = dds_write(response_writer_, &data);
        if (ret == DDS_RETCODE_OK) {
            return true;
        }
        LOG_WRN("[DDS] Response write failed, attempt %d/%d", attempt + 1, max_retries);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}
```

---

## Performance Considerations

### Latency Sources

1. **DDS Transport Overhead**: ~0.1-1ms for local domain communication
2. **Polling Loop**: 1ms sleep adds up with concurrent requests
3. **Tokenization**: Depends on prompt length, typically 1-10ms
4. **Model Inference**: Dominant factor (seconds for LLMs)
5. **Queue Operations**: Minimal, typically <1ms

### Optimization Strategies

1. **Reduce Polling**: Use Waitset instead of sleep-based polling
2. **Batch Requests**: Support multiple concurrent DDS requests
3. **Connection Pooling**: Reuse DDS entities across requests
4. **Zero-Copy**: Minimize data copying between DDS and application
5. **Prioritization**: Use DDS ownership and transport priority QoS

### Benchmark Results Interpretation

The benchmark measures end-to-end latency including:
- Request serialization and DDS publish
- Server receive and processing
- Model inference
- Response serialization and DDS publish
- Client receive and deserialization

Expected improvements over HTTP:
- **Lower protocol overhead**: DDS binary serialization vs HTTP/JSON
- **No connection setup**: Persistent DDS entities
- **Efficient multicast**: Single publish reaches multiple subscribers

---

## Troubleshooting Guide

### Common Issues

#### 1. Client Not Receiving Responses

**Symptoms:**
- Client sends request but never receives response
- Request timeout occurs

**Diagnosis:**
```bash
# Check DDS domain communication
# On Linux:
ddsiconf 0

# Verify topic creation
dds ls -c
```

**Solutions:**
1. Verify QoS settings match between client and server
2. Check domain ID consistency
3. Ensure firewall allows multicast (required for DDS discovery)
4. Verify topic names match exactly

#### 2. High Latency

**Symptoms:**
- Requests take longer than expected
- Latency varies significantly between runs

**Diagnosis:**
```cpp
// Add timing diagnostics
auto start = std::chrono::high_resolution_clock::now();
// ... process request ...
auto end = std::chrono::high_resolution_clock::now();
LOG_DBG("[DDS] Total time: %.2f ms",
    std::chrono::duration<double, std::milli>(end - start).count());
```

**Solutions:**
1. Reduce polling interval (currently 1ms)
2. Use Waitset instead of polling
3. Check for network issues (for multi-host deployments)
4. Verify CycloneDDS shared memory transport is enabled

#### 3. Memory Growth Over Time

**Symptoms:**
- Process memory increases with each request
- Eventually OOM or performance degradation

**Diagnosis:**
```bash
# Monitor memory usage
watch -n 1 ps aux | grep llama-server
# Or use Valgrind:
valgrind --leak-check=full ./llama-server
```

**Solutions:**
1. Fix wrapper cleanup functions
2. Ensure DDS samples are properly freed with `dds_free()`
3. Check for leaks in message array handling

#### 4. Topic Creation Failures

**Symptoms:**
- Server fails to start with DDS enabled
- Error messages about topic creation

**Diagnosis:**
```bash
# Check DDS configuration
cyclonedds-pub -h  # Help for debug tools
```

**Solutions:**
1. Verify CycloneDDS configuration file is readable
2. Check file descriptors limit
3. Ensure no other process has locked the domain

---

## Future Enhancements

### Planned Features

1. **Full Streaming Support**
   - Partial response publishing
   - Client-side streaming callbacks
   - Backpressure handling

2. **Multi-Model Support**
   - DDS topic per model
   - Model discovery via ServerStatus
   - Request routing based on model name

3. **Embedding API**
   - Complete EmbeddingRequest/Response types
   - Optimized batching for embeddings

4. **Security**
   - DDS authentication and encryption
   - Access control lists
   - TLS over DDS

5. **Python Binding**
   - PyPI package
   - Full API wrapper
   - Example notebooks

### Research Areas

1. **Benchmark Comparison**: Systematic HTTP vs DDS comparison with statistical significance
2. **Scalability Testing**: Multi-client, multi-model scenarios
3. **Distributed Deployment**: Multi-host DDS communication
4. **Real-Time Requirements**: Latency guarantees and prioritization

## License

This integration follows the same license as llama.cpp (Apache 2.0).
