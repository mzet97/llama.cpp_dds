#!/bin/bash
# Benchmark script for llama.cpp_dds with GPU (VM version)
# Run on the VM directly (not WSL)

set -e

# Config
BUILD_DIR="/home/oldds/llama.cpp_dds/build"
MODEL="/home/oldds/models/GLM-4.6V-Flash-Q3_K_M.gguf"
PORT=8080
NUM_RUNS=${1:-10}

# Paths
export LD_LIBRARY_PATH="$BUILD_DIR/bin:/home/oldds/cyclonedds/build-install/lib:$LD_LIBRARY_PATH"
export CYCLONEDDS_URI="file:///home/oldds/llama.cpp_dds/dds/cyclonedds-local.xml"

cd /home/oldds/llama.cpp_dds

echo "========================================"
echo "llama.cpp_dds Benchmark with GPU"
echo "========================================"
echo "Model: $MODEL"
echo "GPU: AMD RX 6600M (ROCm)"
echo "Runs: $NUM_RUNS"

# Check GPU
echo ""
echo "[1/6] Checking GPU..."
rocm-smi --showuse || true

# Check model
echo ""
echo "[2/6] Checking model..."
if [ ! -f "$MODEL" ]; then
    echo "ERROR: Model not found at $MODEL"
    exit 1
fi
ls -lh "$MODEL"

# Check build
echo ""
echo "[3/6] Checking build..."
if [ ! -f "$BUILD_DIR/bin/llama-server" ]; then
    echo "ERROR: llama-server not found at $BUILD_DIR/bin/llama-server"
    exit 1
fi
echo "llama-server found"

# Start server with GPU
echo ""
echo "[4/6] Starting server with GPU..."
mkdir -p dds/results
rm -f dds/results/server.log

$BUILD_DIR/bin/llama-server \
    --model "$MODEL" \
    --port $PORT \
    --ctx-size 512 \
    -ngl 99 \
    --enable-dds \
    > dds/results/server.log 2>&1 &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server
echo "[5/6] Waiting for server..."
for i in $(seq 1 60); do
    if curl -s "http://127.0.0.1:$PORT/health" > /dev/null 2>&1; then
        echo "Server ready!"
        break
    fi
    sleep 1
    echo -n "."
done
echo ""

# Check server status
echo ""
echo "Server health:"
curl -s "http://127.0.0.1:$PORT/health" | head -5

# Warmup
echo ""
echo "[6/6] Warmup..."
for i in 1 2 3; do
    curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -d '{"model":"default","messages":[{"role":"user","content":"hi"}],"max_tokens":5}' \
        > /dev/null 2>&1 || true
    echo "Warmup $i done"
done

# Run HTTP benchmark
echo ""
echo "Running HTTP benchmark..."
python3 dds/benchmark_http.py $NUM_RUNS || true

# Run DDS benchmark (if available)
if [ -f "$BUILD_DIR/bin/benchmark_final" ]; then
    echo ""
    echo "Running DDS benchmark..."
    $BUILD_DIR/bin/benchmark_final $NUM_RUNS dds/results/dds_benchmark.csv || true
fi

# Stop server
echo ""
echo "Stopping server..."
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "========================================"
echo "Results"
echo "========================================"

echo ""
echo "HTTP Results:"
cat dds/results/http_results.csv 2>/dev/null || echo "No HTTP results"

echo ""
echo "DDS Results:"
cat dds/results/dds_results.csv 2>/dev/null || echo "No DDS results"

echo ""
echo "Server log (last 30 lines):"
tail -30 dds/results/server.log

echo ""
echo "Benchmark complete!"
