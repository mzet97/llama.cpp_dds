# DDS Support for llama.cpp

This module provides **DDS (Data Distribution Service)** transport support using [Eclipse CycloneDDS](https://github.com/eclipse-cyclonedds/cyclonedds), enabling high-performance, distributed, and real-time communication for `llama.cpp` inference.

## Key Features
- **Low Latency**: ~20% faster than HTTP for small payloads (validated in benchmarks).
- **Interoperability**: Standard IDL-based communication compatible with ROS 2 and other DDS systems.
- **Zero-Copy**: Optimized data path using DDS loan mechanisms.
- **Reliability**: Configurable QoS profiles for reliable or best-effort delivery.

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

Benchmarks run on WSL 2 (TinyLlama-1.1B):

| Prompt Type | DDS Latency (ms) | HTTP Latency (ms) | Improvement |
|-------------|------------------|-------------------|-------------|
| Simple      | **172.55**       | 214.57            | **+19.6%**  |
| Medium      | 545.73           | 540.90            | ~0%         |
| Complex     | 558.26           | 549.61            | ~0%         |

*Note: DDS excels in low-latency control scenarios. For heavy generation tasks, GPU inference time dominates the total latency.*
