# DDS Support for llama.cpp

This module provides **DDS (Data Distribution Service)** transport support using [Eclipse CycloneDDS](https://github.com/eclipse-cyclonedds/cyclonedds), enabling distributed, real-time communication for `llama.cpp` inference.

> For the full technical reference (architecture, QoS, IDL, configuration, troubleshooting), see [docs/dds.md](../docs/dds.md).

## Key Features

- **Alternative to HTTP** — binary CDR over UDP replaces REST/JSON over TCP; no per-request handshake.
- **Streaming** — token-by-token responses via `is_final` flag, mapping to OpenAI SSE semantics.
- **Concurrent inference** — detached thread per request with `atomic<int>` in-flight counter.
- **Built-in discovery** — SPDP/SEDP auto-match readers and writers; no URL configuration.
- **Interoperability** — standard IDL-based types compatible with ROS 2 and other DDS ecosystems.
- **Configurable QoS** — RELIABLE + TRANSIENT_LOCAL for requests/responses; BEST_EFFORT for status.

## Quick Start

### 1. Build CycloneDDS

```bash
git clone https://github.com/eclipse-cyclonedds/cyclonedds.git ~/cyclonedds
cd ~/cyclonedds && mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=~/cyclonedds/install -DCMAKE_BUILD_TYPE=Release
cmake --build . --target install -j$(nproc)
```

### 2. Build llama.cpp with DDS

```bash
cmake -B build -DLLAMA_DDS=ON \
      -DCMAKE_PREFIX_PATH=~/cyclonedds/install \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

For GPU acceleration, add `-DGGML_HIP=ON` (AMD) or `-DGGML_CUDA=ON` (NVIDIA).

See also: [Windows (vcpkg)](../docs/dds.md#windows-vcpkg) | [macOS (Homebrew)](../docs/dds.md#macos-homebrew)

### 3. Run the Server

```bash
./build/bin/llama-server \
    --enable-dds \
    --model models/your-model.gguf \
    --port 8080 \
    -ngl 99
```

The server listens on **both** HTTP (port 8080) and DDS (domain 0).

### 4. Test

```bash
./build/bin/test_client 0 "What is 2+2?"
```

## Benchmark Suite

Three scenarios evaluate DDS vs HTTP under different conditions:

| Scenario | Script | What it measures |
|----------|--------|-----------------|
| **B1** Multi-client | `run_benchmarks_multi.sh` | Throughput + tail latency with 1/2/4/8 concurrent clients |
| **B2** Streaming | `run_benchmarks_stream.sh` | Time-To-First-Token (TTFT) and Inter-Token Latency (ITL) |
| **B3** Network delay | `run_benchmarks_network.sh` | Impact of network latency via `tc netem` |

```bash
# Example: run B1 with 5 iterations
./dds/run_benchmarks_multi.sh 5 dds/results/multi tinyllama
```

### Methodology

- **UUID matching** per request (prevents TRANSIENT_LOCAL cross-contamination)
- **Warmup** excluded from measurements
- **Bessel's correction** for standard deviation
- **Active discovery** via `dds_get_matched_subscriptions()` before timing

## Performance Results

**Environment**: Ryzen 9 5900X · RX 7900 XTX (ROCm 6.4.2, `-ngl 99`) · WSL 2 (Ubuntu 24.04) · TinyLlama 1.1B Q4_K_M · CycloneDDS 0.11.0

### Localhost (N=20)

| Prompt | DDS p50 (ms) | HTTP p50 (ms) | Δ |
|--------|-------------|--------------|---|
| simple | 29.5 | 29.1 | +1.4% |
| medium | 102.8 | 100.4 | +2.4% |
| complex | 96.5 | 92.7 | +4.0% |

On localhost with GPU inference, **DDS ≈ HTTP**. The bottleneck is model inference, not transport.

### With 2 ms Network Delay (N=5, `tc netem`)

| Metric | DDS | HTTP | Δ |
|--------|-----|------|---|
| simple p50 | 31.8 ms | 43.1 ms | **−26%** |
| medium p50 | 104.5 ms | 107.7 ms | −3% |
| stream TTFT (complex) | 20.2 ms | 23.7 ms | **−15%** |
| stream ITL (complex) | 3.2 ms | 3.1 ms | ≈parity |

**DDS wins when transport overhead matters** — short prompts and network delay expose HTTP's per-request TCP handshake cost. For long inference, both transports converge.

### Plots

| Scenario | Directory |
|----------|-----------|
| Single-client | `results/plots_new/` |
| B1 Multi-client | `results/multi/plots/` |
| B2 Streaming | `results/stream/plots/` |
| B3 Network delay | `results/network/plots_netem_*ms/` |

## Directory Structure

```
dds/
├── idl/
│   ├── LlamaDDS.idl          # IDL type definitions (source of truth)
│   ├── LlamaDDS.h             # Generated C types
│   └── LlamaDDS.c             # Generated C serialisation
├── dds_transport.cpp/h        # DDSTransport: participant, topics, read loop
├── dds_bridge.cpp/h           # DDSBridge: thread-safe queue, send_response
├── dds_types.h                # C++ type definitions
├── dds_idl_wrapper.h          # C++ ↔ IDL C conversion, RAII cleanup
├── dds_utils.h                # Thread-safe UUID v4 generator
├── benchmark_final.cpp        # Single-client DDS benchmark
├── benchmark_multi_dds.cpp    # B1: multi-client concurrent
├── benchmark_stream_dds.cpp   # B2: streaming (TTFT/ITL)
├── benchmark_http.py          # Single-client HTTP benchmark
├── benchmark_multi_http.py    # B1: multi-client HTTP
├── benchmark_stream_http.py   # B2: streaming HTTP
├── run_benchmarks_multi.sh    # B1 orchestration
├── run_benchmarks_stream.sh   # B2 orchestration
├── run_benchmarks_network.sh  # B3 orchestration (server/client/netem)
├── plot_benchmarks.py         # Single-client plots
├── plot_multi_benchmarks.py   # B1 plots
├── plot_stream_benchmarks.py  # B2 plots
├── cyclonedds-local.xml       # Localhost config (loopback, no multicast)
├── cyclonedds-network.xml     # Cross-machine config (multicast enabled)
├── cyclonedds-shm-iox.xml     # Shared memory (Iceoryx PSMX)
├── test_client.cpp            # Simple test client
├── CMakeLists.txt             # Build definitions
└── results/                   # Benchmark outputs (CSV + plots)
```

## Further Reading

- [docs/dds.md](../docs/dds.md) — comprehensive technical reference
- [docs/DDS_QUALITY_REPORT.md](../docs/DDS_QUALITY_REPORT.md) — historical quality audit
- [CycloneDDS documentation](https://cyclonedds.io/docs/)
