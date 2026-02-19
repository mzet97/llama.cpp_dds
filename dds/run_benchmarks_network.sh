#!/bin/bash
set -e

# ==========================================================================
# B3 — Network / Distributed Benchmark
#
# Runs benchmarks either:
#  a) Cross-machine: server on machine A, clients on machine B.
#  b) Same-machine with simulated latency via tc netem on loopback.
#
# Usage:
#   Server side:  ./run_benchmarks_network.sh server [gpu_layers] [model_path]
#   Client side:  ./run_benchmarks_network.sh client <server_ip> [num_runs]
#   Simulated:    ./run_benchmarks_network.sh netem [delay_ms] [num_runs] [gpu_layers]
#
# ==========================================================================

MODE="${1:-netem}"

BUILD_DIR="build-wsl"
MODEL="${MODEL:-models/tinyllama.gguf}"
URL="https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
RESULTS_DIR="dds/results/network"

# Use the network XML (no interface binding, multicast on)
export CYCLONEDDS_URI="file://$PWD/dds/cyclonedds-network.xml"

if [ ! -f "$MODEL" ]; then
    echo "Downloading TinyLlama model..."
    wget -q --show-progress -O "$MODEL" "$URL"
fi

mkdir -p "$RESULTS_DIR"

# ============================
# MODE: server
# ============================
if [ "$MODE" = "server" ]; then
    GPU_LAYERS="${2:-0}"
    echo "Starting server in network mode (GPU layers: $GPU_LAYERS)..."
    ./$BUILD_DIR/bin/llama-server \
        --enable-dds \
        --model "$MODEL" \
        --host 0.0.0.0 \
        --port 8080 \
        --ctx-size 512 \
        -ngl "$GPU_LAYERS" \
        2>&1 | tee "$RESULTS_DIR/server.log"
    exit 0
fi

# ============================
# MODE: client  (run on a different machine)
# ============================
if [ "$MODE" = "client" ]; then
    SERVER_IP="${2:?Usage: $0 client <server_ip> [num_runs]}"
    NUM_RUNS="${3:-20}"

    echo "Running client benchmarks against server at $SERVER_IP..."

    # DDS benchmark (uses multicast/unicast discovery — server IP resolved via SPDP)
    echo ""
    echo "=== DDS Benchmark ==="
    ./$BUILD_DIR/bin/benchmark_final "$NUM_RUNS" "$RESULTS_DIR/dds_results.csv" || echo "DDS failed"

    # HTTP benchmark (explicit server IP)
    echo ""
    echo "=== HTTP Benchmark ==="
    python3 dds/benchmark_http.py "$NUM_RUNS" "$RESULTS_DIR/http_results.csv" tinyllama "$SERVER_IP" 8080 || echo "HTTP failed"

    # DDS Streaming
    echo ""
    echo "=== DDS Streaming ==="
    ./$BUILD_DIR/bin/benchmark_stream_dds "$NUM_RUNS" "$RESULTS_DIR/dds_stream.csv" || echo "DDS streaming failed"

    # HTTP Streaming
    echo ""
    echo "=== HTTP Streaming ==="
    python3 dds/benchmark_stream_http.py "$NUM_RUNS" "$RESULTS_DIR/http_stream.csv" tinyllama "$SERVER_IP" 8080 || echo "HTTP streaming failed"

    echo ""
    echo "=== Generating Plots ==="
    if [ -f "$RESULTS_DIR/dds_results.csv" ] && [ -f "$RESULTS_DIR/http_results.csv" ]; then
        python3 dds/plot_benchmarks.py "$RESULTS_DIR/dds_results.csv" "$RESULTS_DIR/http_results.csv" "$RESULTS_DIR/plots"
    fi
    if [ -f "$RESULTS_DIR/dds_stream.csv" ] && [ -f "$RESULTS_DIR/http_stream.csv" ]; then
        python3 dds/plot_stream_benchmarks.py "$RESULTS_DIR/dds_stream.csv" "$RESULTS_DIR/http_stream.csv" "$RESULTS_DIR/plots"
    fi

    echo "Client benchmarks done. Results in $RESULTS_DIR/"
    exit 0
fi

# ============================
# MODE: netem  (simulate network latency on loopback)
# ============================
if [ "$MODE" = "netem" ]; then
    DELAY_MS="${2:-2}"
    JITTER_MS=$(printf "%.1f" "$(echo "$DELAY_MS / 4" | bc -l 2>/dev/null)" 2>/dev/null || echo "0.5")
    NUM_RUNS="${3:-20}"
    GPU_LAYERS="${4:-0}"

    # Use the local XML (since we're still on loopback — netem adds delay)
    export CYCLONEDDS_URI="file://$PWD/dds/cyclonedds-local.xml"

    echo "=== B3 netem: adding ${DELAY_MS}ms ± ${JITTER_MS}ms delay on loopback ==="
    echo "(Requires sudo for tc qdisc)"

    # Add netem delay
    sudo tc qdisc add dev lo root netem delay "${DELAY_MS}ms" "${JITTER_MS}ms" 2>/dev/null || \
    sudo tc qdisc change dev lo root netem delay "${DELAY_MS}ms" "${JITTER_MS}ms"

    cleanup_netem() {
        echo "Removing netem from loopback..."
        sudo tc qdisc del dev lo root 2>/dev/null || true
        if [ -n "$SERVER_PID" ]; then
            echo "Stopping server..."
            kill $SERVER_PID 2>/dev/null || true
            wait $SERVER_PID 2>/dev/null || true
        fi
    }
    trap cleanup_netem EXIT

    # Start server
    echo "Starting server (GPU layers: $GPU_LAYERS)..."
    ./$BUILD_DIR/bin/llama-server \
        --enable-dds \
        --model "$MODEL" \
        --port 8080 \
        --ctx-size 512 \
        -ngl "$GPU_LAYERS" \
        > "$RESULTS_DIR/server_netem.log" 2>&1 &
    SERVER_PID=$!

    # Wait for /health
    echo "Waiting for server..."
    SERVER_READY=0
    for i in $(seq 1 60); do
        if curl --silent --fail --max-time 5 "http://127.0.0.1:8080/health" > /dev/null 2>&1; then
            echo "Server ready!"
            SERVER_READY=1
            break
        fi
        sleep 1
        echo -n "."
    done
    echo ""
    if [ "$SERVER_READY" -eq 0 ]; then
        echo "Server failed! Check $RESULTS_DIR/server_netem.log"
        cat "$RESULTS_DIR/server_netem.log"
        exit 1
    fi

    # Global warmup
    echo "Global warmup (3 requests)..."
    for w in 1 2 3; do
        curl --silent --fail --max-time 30 \
            -X POST "http://127.0.0.1:8080/v1/chat/completions" \
            -H "Content-Type: application/json" \
            -d '{"model":"tinyllama","messages":[{"role":"user","content":"hi"}],"max_tokens":5,"temperature":0.3,"stream":false}' \
            > /dev/null 2>&1 || true
    done

    # DDS benchmark
    echo ""
    echo "=== DDS (netem ${DELAY_MS}ms) ==="
    ./$BUILD_DIR/bin/benchmark_final "$NUM_RUNS" "$RESULTS_DIR/dds_netem_${DELAY_MS}ms.csv" || echo "DDS failed"

    # HTTP benchmark
    echo ""
    echo "=== HTTP (netem ${DELAY_MS}ms) ==="
    python3 dds/benchmark_http.py "$NUM_RUNS" "$RESULTS_DIR/http_netem_${DELAY_MS}ms.csv" || echo "HTTP failed"

    # DDS streaming
    echo ""
    echo "=== DDS Streaming (netem ${DELAY_MS}ms) ==="
    ./$BUILD_DIR/bin/benchmark_stream_dds "$NUM_RUNS" "$RESULTS_DIR/dds_stream_netem_${DELAY_MS}ms.csv" || echo "DDS stream failed"

    # HTTP streaming
    echo ""
    echo "=== HTTP Streaming (netem ${DELAY_MS}ms) ==="
    python3 dds/benchmark_stream_http.py "$NUM_RUNS" "$RESULTS_DIR/http_stream_netem_${DELAY_MS}ms.csv" || echo "HTTP stream failed"

    # Plots
    echo ""
    echo "=== Generating Plots ==="
    PLOT_DIR="$RESULTS_DIR/plots_netem_${DELAY_MS}ms"
    DDS_CSV="$RESULTS_DIR/dds_netem_${DELAY_MS}ms.csv"
    HTTP_CSV="$RESULTS_DIR/http_netem_${DELAY_MS}ms.csv"
    if [ -f "$DDS_CSV" ] && [ -f "$HTTP_CSV" ]; then
        python3 dds/plot_benchmarks.py "$DDS_CSV" "$HTTP_CSV" "$PLOT_DIR"
    fi

    DDS_S_CSV="$RESULTS_DIR/dds_stream_netem_${DELAY_MS}ms.csv"
    HTTP_S_CSV="$RESULTS_DIR/http_stream_netem_${DELAY_MS}ms.csv"
    if [ -f "$DDS_S_CSV" ] && [ -f "$HTTP_S_CSV" ]; then
        python3 dds/plot_stream_benchmarks.py "$DDS_S_CSV" "$HTTP_S_CSV" "$PLOT_DIR"
    fi

    echo ""
    echo "=== netem benchmarks done (delay=${DELAY_MS}ms). Results in $RESULTS_DIR/ ==="
    exit 0
fi

echo "Unknown mode: $MODE"
echo "Usage: $0 {server|client|netem} [args...]"
exit 1
