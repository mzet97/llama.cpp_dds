#!/bin/bash
set -e

# Config
MODEL="models/tinyllama.gguf"
URL="https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
BUILD_DIR="build-wsl"

# 1. Download model if missing
if [ ! -f "$MODEL" ]; then
    echo "Downloading TinyLlama model for benchmark..."
    wget -q --show-progress -O "$MODEL" "$URL"
fi

# 2. Start Server
echo "Starting server with DDS enabled..."
mkdir -p dds/results
./$BUILD_DIR/bin/llama-server --enable-dds --model "$MODEL" --port 8080 --ctx-size 512 > dds/results/server.log 2>&1 &
SERVER_PID=$!

cleanup() {
    echo "Stopping server (PID $SERVER_PID)..."
    kill $SERVER_PID || true
    wait $SERVER_PID || true
}
trap cleanup EXIT

# Wait for server to be ready
echo "Waiting for server to initialize..."
for i in {1..30}; do
    if grep -q "HTTP server listening" dds/results/server.log; then
        echo "Server is ready!"
        break
    fi
    sleep 1
    echo -n "."
done
echo ""

# Verify server is actually running
if ! ps -p $SERVER_PID > /dev/null; then
    echo "Server failed to start! Check dds/results/server.log:"
    cat dds/results/server.log
    exit 1
fi

# 3. Run DDS Benchmark
echo "========================================"
echo "Running DDS Benchmark (C++)"
echo "========================================"
./$BUILD_DIR/bin/benchmark_final 5 dds/results/dds_results.csv || echo "DDS Benchmark failed"

# 4. Run HTTP Benchmark
echo "========================================"
echo "Running HTTP Benchmark (Python)"
echo "========================================"
python3 dds/benchmark_http.py 5 dds/results/http_results.csv || echo "HTTP Benchmark failed"

# 5. Plot Results
echo "========================================"
echo "Generating Comparison Plots"
echo "========================================"
if [ -f "dds/results/dds_results.csv" ] && [ -f "dds/results/http_results.csv" ]; then
    python3 dds/plot_benchmarks.py dds/results/dds_results.csv dds/results/http_results.csv dds/results/plots
else
    echo "Skipping plots: CSV files not found"
fi

echo "========================================"
echo "Benchmarks completed. Results in dds/results/"
