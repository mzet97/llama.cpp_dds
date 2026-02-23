#!/bin/bash
# Benchmark script for llama.cpp_dds with GPU in WSL

export LD_LIBRARY_PATH=/mnt/e/TI/git/tese/llama.cpp_dds/build-wsl/bin:$LD_LIBRARY_PATH
export CYCLONEDDS_URI="file:///mnt/e/TI/git/tese/llama.cpp_dds/dds/cyclonedds-local.xml"

cd /mnt/e/TI/git/tese/llama.cpp_dds

# Use GLM model
MODEL="/mnt/e/TI/git/tese/models/GLM-4.6V-Flash-Q3_K_M.gguf"
PORT=8080

echo "========================================"
echo "llama.cpp_dds Benchmark with GPU"
echo "========================================"
echo "Model: $MODEL"
echo "GPU: AMD RX 7900 XTX (ROCm)"

# Check GPU
echo ""
echo "[1/5] Checking GPU..."
./build-wsl/bin/llama-server --model "$MODEL" -ngl 99 --verbose 2>&1 | grep -E "ROCm|AMD|gfx1100" | head -5

# Start server with GPU
echo ""
echo "[2/5] Starting server with GPU..."
mkdir -p dds/results
./build-wsl/bin/llama-server \
    --model "$MODEL" \
    --port $PORT \
    --ctx-size 512 \
    -ngl 99 \
    --enable-dds \
    > dds/results/server.log 2>&1 &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server
echo "[3/5] Waiting for server..."
for i in $(seq 1 30); do
    if curl -s "http://127.0.0.1:$PORT/health" > /dev/null 2>&1; then
        echo "Server ready!"
        break
    fi
    sleep 1
    echo -n "."
done
echo ""

# Warmup
echo "[4/5] Warmup..."
for i in 1 2 3; do
    curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -d '{"model":"default","messages":[{"role":"user","content":"hi"}],"max_tokens":5}' \
        > /dev/null 2>&1 || true
done

# Run DDS benchmark
echo "[5/5] Running DDS benchmark..."
./build-wsl/bin/benchmark_final 10 dds/results/dds_benchmark.csv || true

# Stop server
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "Results:"
cat dds/results/dds_benchmark.csv

echo ""
echo "Server log:"
tail -20 dds/results/server.log
