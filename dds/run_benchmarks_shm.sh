#!/bin/bash
set -e

###############################################################################
# run_benchmarks_shm.sh — Benchmark with Iceoryx Shared Memory (PSMX)
#
# Usage:  bash dds/run_benchmarks_shm.sh [NUM_ITERATIONS]
#
# Pre-requisites:
#   - iceoryx v2.x installed at ~/iceoryx/install
#   - CycloneDDS rebuilt with ENABLE_ICEORYX=ON at ~/cyclonedds/install
#   - llama.cpp built in build-wsl/ with LLAMA_DDS=ON
###############################################################################

# Fix HOME when invoked from Windows (wsl -e bash -c sets HOME to Windows path)
if [[ "$HOME" == *":"* ]]; then
    export HOME=$(getent passwd "$(whoami)" | cut -d: -f6)
fi

# Config
MODEL="models/tinyllama.gguf"
URL="https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
BUILD_DIR="build-wsl"
NUM_RUNS="${1:-10}"

# Iceoryx / CycloneDDS paths
IOX_PREFIX="$HOME/iceoryx/install"
CDDS_PREFIX="$HOME/cyclonedds/install"
export LD_LIBRARY_PATH="$IOX_PREFIX/lib:$CDDS_PREFIX/lib:${LD_LIBRARY_PATH:-}"
export PATH="$IOX_PREFIX/bin:$PATH"

# Use the SHM-enabled CycloneDDS config
export CYCLONEDDS_URI="file://$PWD/dds/cyclonedds-shm-iox.xml"

echo "========================================"
echo " Iceoryx Shared-Memory Benchmark"
echo "========================================"
echo "  LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
echo "  CYCLONEDDS_URI=$CYCLONEDDS_URI"
echo "  NUM_RUNS=$NUM_RUNS"
echo "========================================"

# 1. Download model if missing
if [ ! -f "$MODEL" ]; then
    echo "Downloading TinyLlama model for benchmark..."
    wget -q --show-progress -O "$MODEL" "$URL"
fi

# 2. Start iox-roudi daemon (shared memory manager)
echo "Starting iox-roudi daemon..."
iox-roudi &
ROUDI_PID=$!
sleep 2  # give RouDi time to create /dev/shm segments

# Verify RouDi is running
if ! kill -0 $ROUDI_PID 2>/dev/null; then
    echo "ERROR: iox-roudi failed to start. Check /dev/shm permissions."
    exit 1
fi
echo "iox-roudi running (PID $ROUDI_PID)"

# Cleanup handler: stop server and RouDi on exit
cleanup() {
    echo ""
    echo "Cleaning up..."
    [ -n "$SERVER_PID" ] && kill $SERVER_PID 2>/dev/null && wait $SERVER_PID 2>/dev/null || true
    kill $ROUDI_PID 2>/dev/null && wait $ROUDI_PID 2>/dev/null || true
    echo "Done."
}
trap cleanup EXIT

# 3. Start Server
echo "Starting server with DDS + SHM enabled..."
mkdir -p dds/results
./$BUILD_DIR/bin/llama-server --enable-dds --model "$MODEL" --port 8080 --ctx-size 512 > dds/results/server_shm.log 2>&1 &
SERVER_PID=$!

# Wait for server to be ready via /health endpoint
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
    echo "Server failed to start! Check dds/results/server_shm.log:"
    tail -30 dds/results/server_shm.log
    exit 1
fi

# Global server warmup: prime model weights, KV-cache allocator and thread pools
echo "Running global server warmup (3 requests discarded)..."
for i in 1 2 3; do
    curl --silent --fail \
         -X POST "http://127.0.0.1:8080/v1/chat/completions" \
         -H "Content-Type: application/json" \
         -d '{"model":"tinyllama","messages":[{"role":"user","content":"hi"}],"max_tokens":5,"temperature":0.3,"stream":false}' \
         > /dev/null 2>&1 || true
done
echo "Warmup done."

# 4. Run DDS (SHM) Benchmark
echo "========================================"
echo "Running DDS Benchmark via Iceoryx SHM (C++)"
echo "========================================"
./$BUILD_DIR/bin/benchmark_final "$NUM_RUNS" dds/results/dds_results_shm.csv || echo "DDS SHM Benchmark failed"

# 5. Run HTTP Benchmark (baseline comparison)
echo "========================================"
echo "Running HTTP Benchmark (Python)"
echo "========================================"
python3 dds/benchmark_http.py "$NUM_RUNS" dds/results/http_results_shm.csv || echo "HTTP Benchmark failed"

# 6. Plot Results
echo "========================================"
echo "Generating Comparison Plots"
echo "========================================"
if [ -f "dds/results/dds_results_shm.csv" ] && [ -f "dds/results/http_results_shm.csv" ]; then
    python3 dds/plot_benchmarks.py dds/results/dds_results_shm.csv dds/results/http_results_shm.csv dds/results/plots_shm
else
    echo "Skipping plots: CSV files not found"
fi

echo "========================================"
echo "SHM Benchmarks completed. Results in dds/results/"
echo "  DDS (SHM): dds/results/dds_results_shm.csv"
echo "  HTTP:      dds/results/http_results_shm.csv"
echo "  Plots:     dds/results/plots_shm/"
echo "========================================"
