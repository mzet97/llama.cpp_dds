# DDS Support for llama.cpp

This module provides **DDS (Data Distribution Service)** transport support using [Eclipse CycloneDDS](https://github.com/eclipse-cyclonedds/cyclonedds), enabling high-performance, distributed, and real-time communication for `llama.cpp` inference.

## Key Features
- **Low Mean Latency**: 10–25% lower mean latency than HTTP across all prompt sizes (see [benchmarks](#performance-results)).
- **Low Median Latency**: Up to 45% faster at the median (p50) for short prompts, where transport overhead dominates.
- **Interoperability**: Standard IDL-based communication compatible with ROS 2 and other DDS ecosystems.
- **Zero-Copy**: Optimised data path using the DDS loan API; avoids redundant copies on the hot path.
- **Configurable Reliability**: QoS profiles for reliable delivery (requests/responses) and best-effort heartbeats (status topic).

## Directory Structure
- `idl/`: DDS Interface Definition Language files (`LlamaDDS.idl`).
- `scripts/`: Helper scripts for building, installing dependencies, and testing.
- `results/`: Benchmark logs, CSV results, and performance plots.
- `benchmark_final.cpp`: C++ benchmarking tool for DDS latency measurement.
- `dds_transport.cpp`: Core transport implementation.

## Requirements

- **CMake** >= 3.16
- **CycloneDDS** >= 0.10.0
- **CycloneDDS C++ Binding** (cyclonedds-cxx)

## Building

### Linux / WSL (Ubuntu)

```bash
# 1. Install CycloneDDS
./dds/scripts/install_dds.sh

# 2. Configure and Build
cmake -B build -DLLAMA_DDS=ON
cmake --build build -j$(nproc)
```

The script installs CycloneDDS from source to `~/cyclonedds/install`. If you have an existing installation, set `CYCLONEDDS_ROOT`:

```bash
cmake -B build -DLLAMA_DDS=ON -DCYCLONEDDS_ROOT=/path/to/cyclonedds/install
cmake --build build -j$(nproc)
```

### Windows (vcpkg)

```powershell
# 1. Install CycloneDDS via vcpkg
vcpkg install cyclonedds:x64-windows
vcpkg integrate install

# 2. Configure and Build
cmake -B build -DLLAMA_DDS=ON `
      -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release -j
```

### macOS (Homebrew)

```bash
# 1. Install CycloneDDS
brew install cyclonedds

# 2. Configure and Build
cmake -B build -DLLAMA_DDS=ON
cmake --build build -j$(sysctl -n hw.logicalcpu)
```

### CMake Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `LLAMA_DDS` | Enable DDS transport | `OFF` |
| `CYCLONEDDS_ROOT` | CycloneDDS install prefix (overrides env var) | auto-detected |
| `CYCLONEDDS_ROOT` env | Alternative to CMake variable | — |

## Usage

### 1. Start the Server
Run the server enabling the DDS transport. It will listen on both HTTP (8080) and DDS (Domain 0).

```bash
./build/bin/llama-server \
    --enable-dds \
    --model models/your-model.gguf \
    --port 8080 \
    --dds-domain 0 \
    --dds-timeout 120    # seconds, default 60
```

### 2. Run Benchmarks
We provide a comprehensive benchmark suite comparing DDS vs HTTP:

```bash
# Run full benchmark suite (DDS C++ client vs HTTP Python client)
./dds/run_benchmarks_wsl.sh
```

This script will:
1. Start the server.
2. Run `benchmark_final` (DDS).
3. Run `benchmark_http.py` (HTTP).
4. Generate comparison plots in `dds/results/plots/`.

## Performance Results

### Environment

- **Platform**: WSL 2 (Ubuntu 22.04 on Windows 11)
- **Model**: TinyLlama-1.1B (GGUF, CPU inference)
- **DDS client**: `benchmark_final` (C++, CycloneDDS)
- **HTTP client**: `benchmark_http.py` (Python, `requests`)
- **Runs per prompt type**: 10
- **Results directory**: `results/` (raw CSV) and `results/plots_new/` (figures)

### Mean latency

| Prompt type | DDS mean ± σ (ms) | HTTP mean ± σ (ms) | Mean improvement |
|-------------|-------------------|--------------------|------------------|
| Simple      | **112.0 ± 121.4** | 149.7 ± 14.4       | **+25.2 %**      |
| Medium      | **457.5 ± 125.3** | 526.9 ± 14.4       | **+13.2 %**      |
| Complex     | **471.6 ± 27.2**  | 525.0 ± 20.1       | **+10.2 %**      |

### Percentile latency (p50 / p95)

| Prompt type | DDS p50 (ms) | DDS p95 (ms) | HTTP p50 (ms) | HTTP p95 (ms) |
|-------------|--------------|--------------|---------------|---------------|
| Simple      | **79.1**     | 458.4        | 145.1         | **188.2**     |
| Medium      | **482.5**    | 619.3        | 523.3         | **550.8**     |
| Complex     | **472.7**    | **514.8**    | 525.1         | 556.5         |

### Interpretation

**DDS consistently wins at the median.** For simple prompts the median is 45% lower than HTTP
(79 ms vs 145 ms), confirming that removing the HTTP framing overhead is significant when the
inference itself is fast.

**DDS has higher tail latency for short and medium prompts.** The p95 for simple prompts
(458 ms) is roughly 2.4× worse than HTTP (188 ms). The wide standard deviation (±121 ms vs
±14 ms) suggests a bimodal distribution — most requests complete quickly, but a minority hit a
colder DDS path (e.g. participant discovery, OS scheduling jitter). For long-running inference
(complex prompts) DDS is better at both mean and p95, because the transport overhead becomes
negligible relative to generation time.

**Takeaway**: DDS is the better choice when median latency matters (real-time control loops,
streaming pipelines) and the workload is dominated by inference time. HTTP provides more
predictable tail latency for latency-SLA-sensitive deployments with short prompts.

### Plots (`results/plots_new/`)

| File | Description |
|------|-------------|
| `latency_mean.png` | Mean latency bar chart: DDS vs HTTP per prompt type |
| `latency_percentiles.png` | p50 / p95 / p99 grouped bar chart |
| `speedup.png` | DDS-over-HTTP speedup factor per prompt type |
| `jitter.png` | Standard deviation comparison (DDS jitter vs HTTP) |
| `summary.png` | Combined summary panel |

> Previous run results and figures are preserved in `results/plots/` for reference.
