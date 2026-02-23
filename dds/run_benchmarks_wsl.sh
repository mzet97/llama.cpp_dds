#!/bin/bash
set -e

# Config
MODEL="models/tinyllama.gguf"
URL="https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
BUILD_DIR="build-wsl"
NUM_RUNS="${1:-10}"  # default 10; override with: ./run_benchmarks_wsl.sh <n>

# Library path for llama.cpp
export LD_LIBRARY_PATH="$PWD/$BUILD_DIR/bin:$LD_LIBRARY_PATH"

# Optimise CycloneDDS for localhost-only communication.
# Both server and benchmark_final (client) inherit this via the environment.
export CYCLONEDDS_URI="file://$PWD/dds/cyclonedds-local.xml"

# 1. Download model if missing
if [ ! -f "$MODEL" ]; then
    echo "Downloading TinyLlama model for benchmark..."
    wget -q --show-progress -O "$MODEL" "$URL"
fi

# 2. Start Server
GPU_LAYERS="${2:-0}"  # default 0 (CPU-only); use 99 for full GPU offload
echo "Starting server with DDS enabled (GPU layers: $GPU_LAYERS)..."
mkdir -p dds/results
./$BUILD_DIR/bin/llama-server --enable-dds --model "$MODEL" --port 8080 --ctx-size 512 -ngl "$GPU_LAYERS" > dds/results/server.log 2>&1 &
SERVER_PID=$!

cleanup() {
    echo "Stopping server (PID $SERVER_PID)..."
    kill $SERVER_PID || true
    wait $SERVER_PID || true
}
trap cleanup EXIT

# Wait for server to be ready via /health endpoint (robust against log format changes)
echo "Waiting for server to initialize..."
SERVER_READY=0
for i in $(seq 1 60); do
    if curl --silent --fail "http://127.0.0.1:8080/health" > /dev/null 2>&1; then
        echo "Server is ready!"
        SERVER_READY=1
        break
    fi
    sleep 1
    echo -n "."
done
echo ""

if [ "$SERVER_READY" -eq 0 ]; then
    echo "Server failed to start or /health not reachable! Check dds/results/server.log:"
    cat dds/results/server.log
    exit 1
fi

# Global server warmup: prime model weights, KV-cache allocator and thread pools
# before any benchmark begins. Avoids cold-start bias in the first prompt.
echo "Running global server warmup (3 requests discarded)..."
for i in 1 2 3; do
    curl --silent --fail \
         -X POST "http://127.0.0.1:8080/v1/chat/completions" \
         -H "Content-Type: application/json" \
         -d '{"model":"tinyllama","messages":[{"role":"user","content":"hi"}],"max_tokens":5,"temperature":0.3,"stream":false}' \
         > /dev/null 2>&1 || true
done
echo "Warmup done. Starting benchmarks..."

# 3. Run DDS Benchmark
echo "========================================"
echo "Running DDS Benchmark (C++)"
echo "========================================"
./$BUILD_DIR/bin/benchmark_final "$NUM_RUNS" dds/results/dds_results.csv || echo "DDS Benchmark failed"

# 4. Run HTTP Benchmark
echo "========================================"
echo "Running HTTP Benchmark (Python)"
echo "========================================"
python3 dds/benchmark_http.py "$NUM_RUNS" dds/results/http_results.csv || echo "HTTP Benchmark failed"

# 5. Plot Results
echo "========================================"
echo "Generating Comparison Plots"
echo "========================================"
if [ -f "dds/results/dds_results.csv" ] && [ -f "dds/results/http_results.csv" ]; then
    python3 dds/plot_benchmarks.py dds/results/dds_results.csv dds/results/http_results.csv dds/results/plots_new
else
    echo "Skipping plots: CSV files not found"
fi

echo "========================================"
echo "Benchmarks completed. Results in dds/results/"
