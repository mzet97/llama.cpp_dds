# DDS Transport Integration for llama.cpp

This document provides comprehensive technical documentation for the DDS (Data Distribution Service) transport layer in llama.cpp, using [Eclipse CycloneDDS](https://github.com/eclipse-cyclonedds/cyclonedds).

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [IDL Definition](#idl-definition)
- [QoS Configuration](#qos-configuration)
- [Building](#building)
- [CycloneDDS XML Configuration](#cyclonedds-xml-configuration)
- [Running](#running)
- [Benchmark Suite](#benchmark-suite)
- [Benchmark Results](#benchmark-results)
- [Troubleshooting](#troubleshooting)
- [File Reference](#file-reference)

## Overview

The DDS transport provides an alternative communication path alongside HTTP for `llama-server`. Instead of REST/JSON over TCP, clients publish and subscribe to strongly-typed DDS topics over UDP (or shared memory), enabling:

- **Low-overhead transport** — binary CDR serialization, no HTTP framing, no TCP handshake per connection.
- **Publish-subscribe** — multiple clients subscribe to the same response topic; the server writes once.
- **Built-in discovery** — SPDP/SEDP automatically match readers and writers; no URL or endpoint configuration needed.
- **Configurable QoS** — RELIABLE delivery with TRANSIENT_LOCAL durability ensures no message loss, while BEST_EFFORT is used for status heartbeats.
- **Streaming support** — partial token-by-token responses via `is_final` flag, mapped to OpenAI-style SSE chunks.
- **Concurrent inference** — the server's DDS poll loop dispatches each request to a detached thread with an `atomic<int>` in-flight counter, mirroring the HTTP threading model.

### When DDS outperforms HTTP

On **localhost** with fast GPU inference, DDS and HTTP achieve near-parity because the bottleneck is inference time, not transport overhead. DDS provides measurable advantages when:

1. **Network latency is non-trivial** — with 2 ms of added network delay (simulated via `tc netem`), DDS reduces simple-prompt p50 latency by **26%** and streaming TTFT by **15%** compared to HTTP, because DDS avoids TCP's 3-way handshake and HTTP framing per request.
2. **Multi-machine distributed deployments** — DDS multicast discovery eliminates the need for service registries or load-balancer configuration.
3. **ROS 2 / robotics integration** — DDS is the native middleware for ROS 2; this integration enables direct LLM inference from ROS 2 nodes without an HTTP bridge.

## Architecture

```
┌───────────────────────────────────────────────────────────────┐
│                      Client Layer                             │
│  benchmark_final.cpp │ benchmark_multi_dds.cpp │ test_client  │
│  benchmark_stream_dds.cpp │ any CycloneDDS C/C++ app         │
└───────────────────────┬───────────────────────────────────────┘
                        │  DDS Topics (UDP / SHM)
                        ▼
┌───────────────────────────────────────────────────────────────┐
│                    DDS Transport Layer                         │
│              DDSTransport  (dds_transport.h/cpp)              │
│  • DomainParticipant lifecycle                                │
│  • Topic creation: request, response, status                  │
│  • DataReader (request) / DataWriter (response, status)       │
│  • WaitSet-based read loop → callback to DDSBridge            │
└───────────────────────┬───────────────────────────────────────┘
                        │
                        ▼
┌───────────────────────────────────────────────────────────────┐
│                     DDS Bridge Layer                           │
│               DDSBridge  (dds_bridge.h/cpp)                   │
│  • Thread-safe request queue  (mutex + condition_variable)    │
│  • pending_requests_ map  (request_id → request)              │
│  • send_response() / send_status()                            │
└───────────────────────┬───────────────────────────────────────┘
                        │
                        ▼
┌───────────────────────────────────────────────────────────────┐
│                  Server Integration                            │
│            server.cpp  — dds_poll_loop()                      │
│  • Drains DDSBridge queue                                     │
│  • Spawns std::thread(...).detach() per request               │
│  • process_dds_request(): tokenize → post task → wait result  │
│  • Streaming: publishes partial chunks (is_final=false)       │
│  • Non-streaming: single response (is_final=true)             │
│  • atomic<int> in_flight for graceful shutdown                │
└───────────────────────────────────────────────────────────────┘
```

### Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| C API (`dds_*`) instead of C++ binding | Avoids CycloneDDS-CXX header-only dependency; simpler build, smaller binary |
| UUID v4 per request (`dds_utils.h`) | Prevents cross-contamination when TRANSIENT_LOCAL history replays old samples |
| Detached threads (not thread pool) | Matches the HTTP server model in llama.cpp; inference is the bottleneck, thread creation cost is negligible |
| WaitSet in client, condition_variable in bridge | Clients need DDS-level blocking; bridge needs cross-thread signaling to the poll loop |

## IDL Definition

The IDL file [`dds/idl/LlamaDDS.idl`](../dds/idl/LlamaDDS.idl) defines all message types in the `llama` module:

### ChatMessage

```idl
struct ChatMessage {
    string role;       // "system", "user", "assistant"
    string content;
};
```

### ChatCompletionRequest

```idl
struct ChatCompletionRequest {
    string request_id;                // UUID v4 for correlation
    string model;                     // e.g. "tinyllama"
    sequence<ChatMessage> messages;
    float temperature;                // 0.0–2.0
    long max_tokens;
    boolean stream;                   // true → server sends partial chunks
    sequence<float> top_p;            // optional
    sequence<long> n;                 // optional
    sequence<string> stop;            // optional stop sequences
};
```

### ChatCompletionResponse

```idl
struct ChatCompletionResponse {
    string request_id;               // matches request
    string model;
    string content;                  // generated text (or chunk)
    string finish_reason;            // "stop", "length", ""
    boolean is_final;                // false for streaming chunks, true for last
    long prompt_tokens;
    long completion_tokens;
};
```

### ServerStatus

```idl
struct ServerStatus {
    string server_id;
    long slots_idle;
    long slots_processing;
    string model_loaded;
    boolean ready;
};
```

### Topics

| Topic Name | Type | Direction |
|------------|------|-----------|
| `llama_chat_completion_request` | `ChatCompletionRequest` | Client → Server |
| `llama_chat_completion_response` | `ChatCompletionResponse` | Server → Client |
| `llama_server_status` | `ServerStatus` | Server → All (heartbeat) |

## QoS Configuration

### Request / Response Topics

```cpp
dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);
```

| Policy | Value | Why |
|--------|-------|-----|
| Reliability | RELIABLE, 10 s timeout | Guarantees delivery; 10 s accommodates slow inference |
| Durability | TRANSIENT_LOCAL | Late-joining readers receive samples published before they joined |
| History | KEEP_LAST 8 | Avoids unbounded memory; 8 slots suffice for concurrent requests |

### Status Topic

```cpp
dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);
dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);
```

Best-effort + volatile is appropriate for periodic heartbeats that can be missed without impact.

### Streaming Responses

For streaming benchmarks, the response reader uses `KEEP_LAST 32` to accommodate the higher chunk rate per request.

## Building

### Prerequisites

| Dependency | Minimum Version | Notes |
|------------|----------------|-------|
| CMake | 3.16 | |
| C++17 compiler | GCC 9+ / Clang 10+ / MSVC 2019+ | |
| CycloneDDS | 0.10.0 | C library |
| CycloneDDS-CXX | — | C++ binding (header-only, found via CMake) |

### Linux / WSL

```bash
# 1. Build CycloneDDS from source (recommended)
git clone https://github.com/eclipse-cyclonedds/cyclonedds.git ~/cyclonedds
cd ~/cyclonedds && mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=~/cyclonedds/install -DCMAKE_BUILD_TYPE=Release
cmake --build . --target install -j$(nproc)

# 2. Build llama.cpp with DDS
cd /path/to/llama.cpp
cmake -B build -DLLAMA_DDS=ON \
      -DCMAKE_PREFIX_PATH=~/cyclonedds/install \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

#### With GPU acceleration (ROCm / HIP)

```bash
cmake -B build -DLLAMA_DDS=ON \
      -DGGML_HIP=ON -DAMDGPU_TARGETS=gfx1100 \
      -DCMAKE_PREFIX_PATH=~/cyclonedds/install \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

#### With GPU acceleration (CUDA)

```bash
cmake -B build -DLLAMA_DDS=ON \
      -DGGML_CUDA=ON \
      -DCMAKE_PREFIX_PATH=~/cyclonedds/install \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Windows (vcpkg)

```powershell
vcpkg install cyclonedds:x64-windows
vcpkg integrate install

cmake -B build -DLLAMA_DDS=ON `
      -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release -j
```

### macOS (Homebrew)

```bash
brew install cyclonedds
cmake -B build -DLLAMA_DDS=ON
cmake --build build -j$(sysctl -n hw.logicalcpu)
```

### CMake Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `LLAMA_DDS` | Enable DDS transport | `OFF` |
| `CYCLONEDDS_ROOT` | CycloneDDS install prefix | auto-detected |
| `GGML_HIP` | Enable AMD GPU (ROCm) | `OFF` |
| `GGML_CUDA` | Enable NVIDIA GPU (CUDA) | `OFF` |

### Build Targets

| Target | Description |
|--------|-------------|
| `llama-server` | Main server with `--enable-dds` flag |
| `test_client` | Simple DDS test client |
| `benchmark_final` | Single-client latency benchmark |
| `benchmark_multi_dds` | Multi-client concurrent benchmark |
| `benchmark_stream_dds` | Streaming (TTFT/ITL) benchmark |

## CycloneDDS XML Configuration

CycloneDDS behaviour is controlled via an XML config file, pointed to by the `CYCLONEDDS_URI` environment variable.

### Localhost (low-latency)

[`dds/cyclonedds-local.xml`](../dds/cyclonedds-local.xml) — binds to loopback, disables multicast, aggressive heartbeat tuning:

```xml
<Domain id="any">
    <General>
        <Interfaces>
            <NetworkInterface address="127.0.0.1" />
        </Interfaces>
        <AllowMulticast>false</AllowMulticast>
    </General>
    <Discovery>
        <MaxAutoParticipantIndex>9</MaxAutoParticipantIndex>
    </Discovery>
    <Internal>
        <HeartbeatInterval>10ms</HeartbeatInterval>
        <WriterLingerDuration>0ms</WriterLingerDuration>
        <NackDelay>0ms</NackDelay>
    </Internal>
</Domain>
```

### Cross-machine / Network

[`dds/cyclonedds-network.xml`](../dds/cyclonedds-network.xml) — no interface binding, multicast enabled, higher participant limit:

```xml
<Domain id="any">
    <General>
        <AllowMulticast>true</AllowMulticast>
    </General>
    <Discovery>
        <MaxAutoParticipantIndex>30</MaxAutoParticipantIndex>
    </Discovery>
    <Internal>
        <HeartbeatInterval>10ms</HeartbeatInterval>
        <WriterLingerDuration>0ms</WriterLingerDuration>
        <NackDelay>0ms</NackDelay>
    </Internal>
</Domain>
```

### Shared Memory (Iceoryx PSMX)

[`dds/cyclonedds-shm-iox.xml`](../dds/cyclonedds-shm-iox.xml) — requires CycloneDDS built with `ENABLE_ICEORYX=ON` and Iceoryx v2.0.6+ RouDi running. Bypasses the network stack entirely for same-host communication. Note: benchmarks show no measurable advantage over loopback UDP when the bottleneck is model inference.

### Applying a configuration

```bash
export CYCLONEDDS_URI=file://$(pwd)/dds/cyclonedds-local.xml
```

## Running

### Start the Server with DDS

```bash
./build/bin/llama-server \
    --enable-dds \
    --model models/your-model.gguf \
    --port 8080 \
    -ngl 99   # GPU layers (optional)
```

The server listens on **both** HTTP (port 8080) and DDS (domain 0) simultaneously.

### Test Client

```bash
# Send a single request and print the response
./build/bin/test_client 0 "What is 2+2?"
```

### Single-Client Benchmark

```bash
# 20 iterations per prompt type, using model name "tinyllama"
./build/bin/benchmark_final 20 dds/results tinyllama
```

## Benchmark Suite

The project includes three benchmark scenarios designed to evaluate DDS under different conditions. Each scenario tests both DDS (C++ client) and HTTP (Python client) against the same server.

### B1 — Multi-Client Concurrent

Tests throughput and tail latency under concurrent load with 1, 2, 4, and 8 simultaneous clients.

```bash
./dds/run_benchmarks_multi.sh [N] [RESULTS_DIR] [MODEL]
# e.g. ./dds/run_benchmarks_multi.sh 5 dds/results/multi tinyllama
```

**Produces**: CSV files per client count + 5 plots (throughput, p95, mean, boxplot, heatmap).

### B2 — Streaming (TTFT / ITL)

Measures Time-To-First-Token (TTFT) and Inter-Token Latency (ITL) for streaming responses.

```bash
./dds/run_benchmarks_stream.sh [N] [RESULTS_DIR] [MODEL]
```

**Produces**: CSV files + 4 plots (TTFT comparison, ITL comparison, total time, ITL CDF).

### B3 — Network Delay (tc netem)

Simulates real network conditions using Linux `tc netem` to add configurable delay. Requires root.

```bash
# On the server host:
sudo ./dds/run_benchmarks_network.sh server [N] [RESULTS_DIR] [MODEL]

# On a separate terminal (or remote host), add delay:
sudo ./dds/run_benchmarks_network.sh netem 2   # adds 2ms delay

# On the client host:
sudo ./dds/run_benchmarks_network.sh client [N] [RESULTS_DIR] [MODEL]
```

**Produces**: CSV files + 8 plots (simple/stream for DDS and HTTP, with delay analysis).

### Benchmark Methodology

The benchmark suite incorporates the following practices to ensure validity:

| Practice | Implementation |
|----------|---------------|
| **UUID matching** | Each request gets a UUID v4; responses with non-matching `request_id` are discarded (prevents cross-contamination from TRANSIENT_LOCAL history) |
| **Warmup** | 2 warmup requests per prompt type are excluded from measurements |
| **Bessel's correction** | Standard deviation uses `N-1` denominator for unbiased estimator |
| **Active discovery** | `dds_get_matched_subscriptions()` polls until the response reader is matched before sending |
| **Configurable model** | Model name passed via CLI to avoid hardcoded mismatches |
| **No inter-iteration sleep** | Removed artificial delays between iterations |

## Benchmark Results

All results below were collected on:

- **CPU**: AMD Ryzen 9 5900X (12-core)
- **GPU**: AMD Radeon RX 7900 XTX (24 GB VRAM, ROCm 6.4.2, `-ngl 99`)
- **OS**: WSL 2 (Ubuntu 24.04)
- **Model**: TinyLlama 1.1B Chat v1.0 Q4_K_M (637 MB)
- **CycloneDDS**: 0.11.0 with `cyclonedds-local.xml`

### Single-Client Localhost (N=20)

| Prompt | DDS p50 (ms) | HTTP p50 (ms) | Δ |
|--------|-------------|--------------|---|
| simple | 29.5 | 29.1 | +1.4% |
| medium | 102.8 | 100.4 | +2.4% |
| complex | 96.5 | 92.7 | +4.0% |

**Interpretation**: On localhost with GPU inference, DDS and HTTP achieve near-parity. The bottleneck is model inference (~30–100 ms), not transport overhead.

### B2 — Streaming (N=5)

| Metric | DDS | HTTP |
|--------|-----|------|
| TTFT (complex) | 18.3 ms | 16.3 ms |
| ITL (complex) | 3.3 ms | 3.2 ms |
| Total (complex) | 349 ms | 336 ms |

**Interpretation**: On localhost, streaming latency is at parity. DDS adds ~2 ms TTFT overhead from discovery/matching, which is negligible for real workloads.

### B3 — Network Delay: 2 ms (N=5)

This is where DDS differentiates. Adding just 2 ms of network delay (via `tc netem`) penalises HTTP's per-request TCP handshake, while DDS's persistent UDP association absorbs the delay once at discovery time.

| Metric | DDS | HTTP | Δ |
|--------|-----|------|---|
| simple p50 | 31.8 ms | 43.1 ms | **−26%** |
| medium p50 | 104.5 ms | 107.7 ms | −3% |
| complex p50 | 99.6 ms | 100.4 ms | −1% |
| stream TTFT (complex) | 20.2 ms | 23.7 ms | **−15%** |
| stream ITL (complex) | 3.2 ms | 3.1 ms | ≈parity |

**Key takeaway**: DDS's advantage scales with transport overhead relative to inference time. For short prompts (where inference is fast), the 3-way TCP handshake overhead that HTTP pays per request becomes significant. For complex prompts with long inference, both transports converge because inference dominates.

### Plot Directories

| Scenario | Directory |
|----------|-----------|
| Single-client | `dds/results/plots_new/` |
| B1 Multi-client | `dds/results/multi/plots/` |
| B2 Streaming | `dds/results/stream/plots/` |
| B3 Network delay | `dds/results/network/plots_netem_*ms/` |

## Troubleshooting

### Client not receiving responses

**Cause**: QoS mismatch — client and server must use identical RELIABILITY, DURABILITY, and HISTORY settings.

**Fix**: Ensure the client creates QoS with:
```cpp
dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);
```

### Stale responses from previous runs

**Cause**: TRANSIENT_LOCAL durability replays old samples to new readers.

**Fix**: Each request uses a UUID v4 (`dds_utils.h`). Clients must filter responses by `request_id`. All benchmark clients already do this.

### Discovery takes too long

**Cause**: Multicast disabled or blocked by firewall; SPDP cannot find peers.

**Fix**:
1. For localhost, use `cyclonedds-local.xml` which binds to `127.0.0.1`.
2. For cross-machine, ensure multicast is allowed on the network interface.
3. Use `dds_get_matched_subscriptions()` to actively wait for discovery before sending requests (all benchmark clients do this).

### High tail latency on first request

**Cause**: DDS participant discovery (SPDP) runs asynchronously; the first request may arrive before the response writer is matched.

**Fix**: Benchmark clients poll `dds_get_matched_subscriptions()` in a loop before timing begins. For production clients, implement the same pattern.

### WSL 2 specifics

- WSL 2 uses a Hyper-V virtual NIC; cross-host traffic goes through the virtual bridge, adding ~0.5 ms jitter.
- For best localhost results, use `cyclonedds-local.xml` with `127.0.0.1` binding.
- Shared memory (Iceoryx PSMX) works within WSL 2 but requires RouDi daemon running.

## File Reference

### Core Implementation

| File | Purpose |
|------|---------|
| `dds/dds_transport.cpp` | DDSTransport — DDS participant, topics, read loop, response/status writers |
| `dds/dds_transport.h` | Transport public interface (pimpl) |
| `dds/dds_bridge.cpp` | DDSBridge — thread-safe request queue, send_response, condition_variable |
| `dds/dds_bridge.h` | Bridge public interface |
| `dds/dds_types.h` | C++ type definitions (ChatCompletionRequest, ChatCompletionResponse, etc.) |
| `dds/dds_idl_wrapper.h` | Conversion helpers: C++ types ↔ IDL C structs, RAII cleanup |
| `dds/dds_utils.h` | Thread-safe UUID v4 generator |
| `dds/idl/LlamaDDS.idl` | IDL type definitions (source of truth) |
| `dds/idl/LlamaDDS.h` | Generated C types (from `idlc`) |
| `dds/idl/LlamaDDS.c` | Generated C serialisation |
| `tools/server/server.cpp` | Server integration: `dds_poll_loop()`, `process_dds_request()` |

### Benchmarks

| File | Purpose |
|------|---------|
| `dds/benchmark_final.cpp` | Single-client DDS benchmark (UUID, warmup, Bessel stddev) |
| `dds/benchmark_http.py` | Single-client HTTP benchmark (Python, configurable host/port) |
| `dds/benchmark_multi_dds.cpp` | B1: multi-client concurrent DDS benchmark |
| `dds/benchmark_multi_http.py` | B1: multi-client concurrent HTTP benchmark |
| `dds/benchmark_stream_dds.cpp` | B2: streaming DDS benchmark (TTFT/ITL) |
| `dds/benchmark_stream_http.py` | B2: streaming HTTP benchmark (fresh connection per request) |
| `dds/run_benchmarks_multi.sh` | B1 orchestration (1/2/4/8 clients) |
| `dds/run_benchmarks_stream.sh` | B2 orchestration |
| `dds/run_benchmarks_network.sh` | B3 orchestration (server/client/netem modes) |
| `dds/plot_benchmarks.py` | Plotting for single-client results |
| `dds/plot_multi_benchmarks.py` | Plotting for B1 (throughput, p95, mean, boxplot) |
| `dds/plot_stream_benchmarks.py` | Plotting for B2 (TTFT, ITL, total, CDF) |

### Configuration

| File | Purpose |
|------|---------|
| `dds/cyclonedds-local.xml` | Localhost optimised (loopback, no multicast, aggressive heartbeat) |
| `dds/cyclonedds-network.xml` | Cross-machine (multicast enabled, no interface binding) |
| `dds/cyclonedds-shm-iox.xml` | Shared memory via Iceoryx PSMX plugin |
| `dds/CMakeLists.txt` | Build definitions for llama-dds library and benchmark targets |

### Documentation

| File | Purpose |
|------|---------|
| `dds/README.md` | Quick-start guide and overview |
| `docs/dds.md` | This file — comprehensive technical reference |
| `docs/DDS_QUALITY_REPORT.md` | Historical quality audit (initial refactoring phase) |
