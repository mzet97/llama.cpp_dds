#!/bin/bash
set -e

# ==========================================================================
# B2 — Streaming Benchmark (TTFT & Inter-Token Latency)
#
# Usage: ./run_benchmarks_stream.sh [num_runs] [gpu_layers] [model_path]
# ==========================================================================

MODEL="${3:-models/tinyllama.gguf}"
URL="https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
BUILD_DIR="build-wsl"
NUM_RUNS="${1:-20}"
GPU_LAYERS="${2:-0}"
RESULTS_DIR="dds/results/stream"

export CYCLONEDDS_URI="file://$PWD/dds/cyclonedds-local.xml"

# --------------------------------------------------------------------------
# 1. Download model
# --------------------------------------------------------------------------
if [ ! -f "$MODEL" ]; then
    echo "Downloading TinyLlama model..."
    wget -q --show-progress -O "$MODEL" "$URL"
fi

# --------------------------------------------------------------------------
# 2. Start server
# --------------------------------------------------------------------------
echo "Starting server (GPU layers: $GPU_LAYERS)..."
mkdir -p "$RESULTS_DIR"
./$BUILD_DIR/bin/llama-server \
    --enable-dds \
    --model "$MODEL" \
    --port 8080 \
    --ctx-size 512 \
    -ngl "$GPU_LAYERS" \
    > "$RESULTS_DIR/server.log" 2>&1 &
SERVER_PID=$!

cleanup() {
    echo "Stopping server (PID $SERVER_PID)..."
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
}
trap cleanup EXIT

# Wait for /health
echo "Waiting for server..."
SERVER_READY=0
for i in $(seq 1 60); do
    if curl --silent --fail "http://127.0.0.1:8080/health" > /dev/null 2>&1; then
        echo "Server ready!"
        SERVER_READY=1
        break
    fi
    sleep 1
    echo -n "."
done
echo ""
if [ "$SERVER_READY" -eq 0 ]; then
    echo "Server failed! Check $RESULTS_DIR/server.log"
    cat "$RESULTS_DIR/server.log"
    exit 1
fi

# Global warmup
echo "Global warmup (3 requests)..."
for w in 1 2 3; do
    curl --silent --fail \
        -X POST "http://127.0.0.1:8080/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -d '{"model":"tinyllama","messages":[{"role":"user","content":"hi"}],"max_tokens":5,"temperature":0.3,"stream":false}' \
        > /dev/null 2>&1 || true
done
echo "Warmup done."

# --------------------------------------------------------------------------
# 3. DDS Streaming
# --------------------------------------------------------------------------
echo ""
echo "================================================================="
echo "  DDS Streaming Benchmark"
echo "================================================================="
./$BUILD_DIR/bin/benchmark_stream_dds "$NUM_RUNS" "$RESULTS_DIR/dds_stream.csv" || echo "DDS streaming failed"

# --------------------------------------------------------------------------
# 4. HTTP Streaming
# --------------------------------------------------------------------------
echo ""
echo "================================================================="
echo "  HTTP Streaming Benchmark"
echo "================================================================="
python3 dds/benchmark_stream_http.py "$NUM_RUNS" "$RESULTS_DIR/http_stream.csv" || echo "HTTP streaming failed"

# --------------------------------------------------------------------------
# 5. Plot
# --------------------------------------------------------------------------
echo ""
echo "================================================================="
echo "  Generating Streaming Plots"
echo "================================================================="
if [ -f "$RESULTS_DIR/dds_stream.csv" ] && [ -f "$RESULTS_DIR/http_stream.csv" ]; then
    python3 dds/plot_stream_benchmarks.py "$RESULTS_DIR/dds_stream.csv" "$RESULTS_DIR/http_stream.csv" "$RESULTS_DIR/plots"
else
    echo "Skipping plots: CSV files not found"
fi

echo ""
echo "================================================================="
echo "Streaming benchmarks complete. Results in $RESULTS_DIR/"
echo "================================================================="
