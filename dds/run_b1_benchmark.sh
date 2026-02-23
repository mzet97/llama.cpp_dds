#!/bin/bash
# B1 Multi-Client Benchmark Script

export LD_LIBRARY_PATH="/home/oldds/llama.cpp_dds/build/bin:/home/oldds/cyclonedds/build-install/lib:$LD_LIBRARY_PATH"
export CYCLONEDDS_URI="file:///home/oldds/llama.cpp_dds/dds/cyclonedds-local.xml"

MODEL="/home/oldds/models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
PORT=8096
NUM_RUNS=${1:-5}

cd /home/oldds/llama.cpp_dds/dds

echo "=========================================="
echo "B1: Multi-Client Benchmark"
echo "=========================================="
echo "Model: $MODEL"
echo "Port: $PORT"
echo "Runs: $NUM_RUNS"

# Kill any existing server on this port
pkill -f "llama-server.*$PORT" 2>/dev/null || true
sleep 2

# Start server
echo ""
echo "[1/4] Starting server..."
mkdir -p results/multi
/home/oldds/llama.cpp_dds/build/bin/llama-server \
    --model "$MODEL" \
    --port $PORT \
    --ctx-size 512 \
    -ngl 99 \
    --enable-dds \
    > results/multi/server.log 2>&1 &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server
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
echo "[2/4] Warmup..."
for i in 1 2 3; do
    curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -d '{"model":"tinyllama","messages":[{"role":"user","content":"hi"}],"max_tokens":5}' \
        > /dev/null 2>&1 || true
done
echo "Warmup done"

# Run HTTP benchmark
echo "[3/4] Running HTTP benchmark..."
python3 benchmark_multi_http.py $NUM_RUNS 127.0.0.1 tinyllama $PORT || true

# Run DDS benchmark
echo "[4/4] Running DDS benchmark..."
/home/oldds/llama.cpp_dds/build/bin/benchmark_multi_dds $NUM_RUNS results/multi/dds_results.csv || true

# Stop server
kill $SERVER_PID 2>/dev/null || true

echo ""
echo "=========================================="
echo "Results"
echo "=========================================="
echo ""
echo "HTTP Results:"
cat results/multi/http_results.csv 2>/dev/null || echo "No HTTP results"
echo ""
echo "DDS Results:"
cat results/multi/dds_results.csv 2>/dev/null || echo "No DDS results"
echo ""
echo "Done!"
