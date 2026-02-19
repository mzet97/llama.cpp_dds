#!/bin/bash
set -e

# ==========================================================================
# B1 — Multi-Client Concurrent Benchmark
#
# Runs DDS and HTTP benchmarks with 1, 2, 4 and 8 concurrent clients.
# Server must be started with --parallel 8 to allow concurrent inference.
#
# Usage: ./run_benchmarks_multi.sh [num_runs] [gpu_layers] [model_path]
# ==========================================================================

MODEL="${3:-models/tinyllama.gguf}"
URL="https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
BUILD_DIR="build-wsl"
NUM_RUNS="${1:-20}"
GPU_LAYERS="${2:-0}"
PARALLEL_SLOTS=8
RESULTS_DIR="dds/results/multi"

export CYCLONEDDS_URI="file://$PWD/dds/cyclonedds-local.xml"

# --------------------------------------------------------------------------
# 1. Download model if missing
# --------------------------------------------------------------------------
if [ ! -f "$MODEL" ]; then
    echo "Downloading TinyLlama model..."
    wget -q --show-progress -O "$MODEL" "$URL"
fi

# --------------------------------------------------------------------------
# 2. Start server with concurrent inference slots
# --------------------------------------------------------------------------
echo "Starting server (GPU layers: $GPU_LAYERS, parallel slots: $PARALLEL_SLOTS)..."
mkdir -p "$RESULTS_DIR"
./$BUILD_DIR/bin/llama-server \
    --enable-dds \
    --model "$MODEL" \
    --port 8080 \
    --ctx-size 512 \
    --parallel "$PARALLEL_SLOTS" \
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

# Global server warmup
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
# 3. Run multi-client sweeps
# --------------------------------------------------------------------------
CLIENT_COUNTS="1 2 4 8"

for CLIENTS in $CLIENT_COUNTS; do
    echo ""
    echo "================================================================="
    echo "  DDS  — $CLIENTS concurrent client(s), $NUM_RUNS runs each"
    echo "================================================================="

    # Remove old per-client CSVs
    rm -f "$RESULTS_DIR"/dds_client_*_n${CLIENTS}.csv

    PIDS=""
    for c in $(seq 1 $CLIENTS); do
        ./$BUILD_DIR/bin/benchmark_multi_dds \
            "$NUM_RUNS" \
            "$RESULTS_DIR/dds_client_${c}_n${CLIENTS}.csv" \
            tinyllama \
            "$c" &
        PIDS="$PIDS $!"
    done
    wait $PIDS
    echo "DDS $CLIENTS-client done."

    echo ""
    echo "================================================================="
    echo "  HTTP — $CLIENTS concurrent client(s), $NUM_RUNS runs each"
    echo "================================================================="

    rm -f "$RESULTS_DIR"/http_client_*_n${CLIENTS}.csv

    PIDS=""
    for c in $(seq 1 $CLIENTS); do
        python3 dds/benchmark_multi_http.py \
            "$NUM_RUNS" \
            "$RESULTS_DIR/http_client_${c}_n${CLIENTS}.csv" \
            tinyllama \
            "$c" &
        PIDS="$PIDS $!"
    done
    wait $PIDS
    echo "HTTP $CLIENTS-client done."
done

# --------------------------------------------------------------------------
# 4. Aggregate CSVs  (one file per transport×client-count)
# --------------------------------------------------------------------------
echo ""
echo "Aggregating results..."
for CLIENTS in $CLIENT_COUNTS; do
    for TRANSPORT in dds http; do
        AGG="$RESULTS_DIR/${TRANSPORT}_multi_n${CLIENTS}.csv"
        head -1 "$RESULTS_DIR/${TRANSPORT}_client_1_n${CLIENTS}.csv" > "$AGG"
        for c in $(seq 1 $CLIENTS); do
            tail -n +2 "$RESULTS_DIR/${TRANSPORT}_client_${c}_n${CLIENTS}.csv" >> "$AGG"
        done
    done
done
echo "Aggregation done."

# --------------------------------------------------------------------------
# 5. Plot results
# --------------------------------------------------------------------------
echo ""
echo "Generating plots..."
python3 dds/plot_multi_benchmarks.py "$RESULTS_DIR" "$RESULTS_DIR/plots"

echo ""
echo "================================================================="
echo "Multi-client benchmarks complete. Results in $RESULTS_DIR/"
echo "================================================================="
