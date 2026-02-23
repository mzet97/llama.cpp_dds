#!/bin/bash
# B2 Streaming Benchmark Script (TTFT & ITL)

export LD_LIBRARY_PATH="/home/oldds/llama.cpp_dds/build/bin:/home/oldds/cyclonedds/build-install/lib:$LD_LIBRARY_PATH"
export CYCLONEDDS_URI="file:///home/oldds/llama.cpp_dds/dds/cyclonedds-local.xml"

MODEL="/home/oldds/models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
PORT=8097

cd /home/oldds/llama.cpp_dds/dds

echo "=========================================="
echo "B2: Streaming Benchmark (TTFT & ITL)"
echo "=========================================="
echo "Model: $MODEL"
echo "Port: $PORT"

# Kill existing
pkill -f "llama-server.*$PORT" 2>/dev/null || true
sleep 2

# Start server
echo ""
echo "[1/3] Starting server..."
mkdir -p results/stream
/home/oldds/llama.cpp_dds/build/bin/llama-server \
    --model "$MODEL" \
    --port $PORT \
    --ctx-size 512 \
    -ngl 99 \
    --enable-dds \
    > results/stream/server.log 2>&1 &

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
echo "[2/3] Warmup..."
curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"tinyllama","messages":[{"role":"user","content":"hi"}],"max_tokens":5}' \
    > /dev/null 2>&1 || true

# Run DDS streaming benchmark
echo "[3/3] Running DDS streaming benchmark..."
/home/oldds/llama.cpp_dds/build/bin/benchmark_stream_dds 10 results/stream/dds_stream.csv || true

# Stop server
kill $SERVER_PID 2>/dev/null || true

echo ""
echo "=========================================="
echo "Results"
echo "=========================================="
cat results/stream/dds_stream.csv 2>/dev/null || echo "No results"
echo ""
echo "Done!"
