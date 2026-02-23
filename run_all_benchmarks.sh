#!/bin/bash
# =============================================================================
# Master Benchmark Runner — FASE 2.1 / 2.2 / 2.3
# =============================================================================
# Rebuilds llama-server with the poll-interval fix (FASE 2.1: 50ms → 5000ms
# blocking on condition variable), then runs B0/B1/B2 DDS benchmarks and
# an HTTP baseline comparison.
#
# Usage:
#   bash run_all_benchmarks.sh [--skip-rebuild] [--model MODEL_PATH] [--runs N]
#
# Prerequisites (WSL):
#   - ROCm / HIP installed
#   - CycloneDDS installed at ~/cyclonedds/install
#   - gguf model file available
# =============================================================================

set -euo pipefail

# ── Defaults ─────────────────────────────────────────────────────────────────
BASE_DIR="/mnt/e/TI/git/tese/llama.cpp_dds"
BUILD_DIR="${BASE_DIR}/build-wsl"
BIN_DIR="${BUILD_DIR}/bin"
RESULTS_DIR="${BASE_DIR}/dds/results"
MODEL="/mnt/e/TI/git/tese/models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
NUM_RUNS=10
SKIP_REBUILD=false
PORT=8080
CTX_SIZE=512
N_GPU_LAYERS=99  # offload all layers to GPU

# ── Parse arguments ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-rebuild) SKIP_REBUILD=true; shift ;;
        --model)        MODEL="$2"; shift 2 ;;
        --runs)         NUM_RUNS="$2"; shift 2 ;;
        --port)         PORT="$2"; shift 2 ;;
        *)              echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ── Environment ──────────────────────────────────────────────────────────────
export LD_LIBRARY_PATH="${BIN_DIR}:/home/zet/cyclonedds/install/lib:${LD_LIBRARY_PATH:-}"
export CYCLONEDDS_URI="file://${BASE_DIR}/dds/cyclonedds-local.xml"

mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG="${RESULTS_DIR}/benchmark_${TIMESTAMP}.log"

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

# ── Helper: kill server ──────────────────────────────────────────────────────
kill_server() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
    # Also kill any lingering llama-server
    pkill -f "llama-server.*--port ${PORT}" 2>/dev/null || true
    sleep 1
}
trap kill_server EXIT

# =============================================================================
# STEP 1: Rebuild (server only — incremental)
# =============================================================================
if [[ "$SKIP_REBUILD" == false ]]; then
    log "═══════════════════════════════════════════════════════"
    log "STEP 1: Rebuilding llama-server (poll-interval fix)"
    log "═══════════════════════════════════════════════════════"

    cd "$BUILD_DIR"
    cmake --build . --target llama-server benchmark_final benchmark_multi_dds benchmark_stream_dds -j$(nproc) 2>&1 | tail -20 | tee -a "$LOG"

    log "Build complete. Binary: ${BIN_DIR}/llama-server"
    log "  benchmark_final:      $(stat -c%Y ${BIN_DIR}/benchmark_final 2>/dev/null || echo 'missing')"
    log "  benchmark_stream_dds: $(stat -c%Y ${BIN_DIR}/benchmark_stream_dds 2>/dev/null || echo 'missing')"
    log "  benchmark_multi_dds:  $(stat -c%Y ${BIN_DIR}/benchmark_multi_dds 2>/dev/null || echo 'missing')"
else
    log "STEP 1: Skipping rebuild (--skip-rebuild)"
fi

cd "$BASE_DIR"

# =============================================================================
# STEP 2: Start llama-server with DDS + GPU
# =============================================================================
log ""
log "═══════════════════════════════════════════════════════"
log "STEP 2: Starting llama-server"
log "═══════════════════════════════════════════════════════"
log "  Model:      $MODEL"
log "  Port:       $PORT"
log "  GPU layers: $N_GPU_LAYERS"
log "  Ctx size:   $CTX_SIZE"

kill_server  # ensure no stale server

"${BIN_DIR}/llama-server" \
    --model "$MODEL" \
    --port "$PORT" \
    --ctx-size "$CTX_SIZE" \
    -ngl "$N_GPU_LAYERS" \
    --enable-dds \
    > "${RESULTS_DIR}/server_${TIMESTAMP}.log" 2>&1 &
SERVER_PID=$!
log "  Server PID: $SERVER_PID"

# Wait for server health
log "  Waiting for server..."
for i in $(seq 1 60); do
    if curl -s "http://127.0.0.1:${PORT}/health" 2>/dev/null | grep -q '"status"'; then
        log "  Server ready! (${i}s)"
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        log "  ERROR: Server process died!"
        tail -20 "${RESULTS_DIR}/server_${TIMESTAMP}.log" | tee -a "$LOG"
        exit 1
    fi
    sleep 1
done

# Verify server is really responding
HEALTH=$(curl -s "http://127.0.0.1:${PORT}/health" 2>/dev/null || echo "FAIL")
log "  Health: $HEALTH"

# =============================================================================
# STEP 3: HTTP Baseline (to compare with DDS)
# =============================================================================
log ""
log "═══════════════════════════════════════════════════════"
log "STEP 3: HTTP Baseline Benchmark"
log "═══════════════════════════════════════════════════════"

HTTP_CSV="${RESULTS_DIR}/http_baseline_${TIMESTAMP}.csv"
echo "prompt_type,run,latency_ms" > "$HTTP_CSV"

# Warmup
for i in 1 2 3; do
    curl -s -X POST "http://127.0.0.1:${PORT}/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -d '{"model":"default","messages":[{"role":"user","content":"hi"}],"max_tokens":5}' \
        > /dev/null 2>&1 || true
done
log "  Warmup done"

for PROMPT_NAME in "simple" "medium" "complex"; do
    case "$PROMPT_NAME" in
        simple)  PROMPT="What is 2+2?"; MAX_TOK=30 ;;
        medium)  PROMPT="Explain machine learning in a few sentences."; MAX_TOK=30 ;;
        complex) PROMPT="Write a detailed technical explanation of how neural networks work, including backpropagation, gradient descent, and the role of activation functions."; MAX_TOK=30 ;;
    esac

    log "  HTTP [$PROMPT_NAME] ($NUM_RUNS runs, max_tokens=$MAX_TOK)"

    for i in $(seq 1 "$NUM_RUNS"); do
        START_NS=$(date +%s%N)
        curl -s -X POST "http://127.0.0.1:${PORT}/v1/chat/completions" \
            -H "Content-Type: application/json" \
            -d "{\"model\":\"default\",\"messages\":[{\"role\":\"user\",\"content\":\"${PROMPT}\"}],\"max_tokens\":${MAX_TOK}}" \
            > /dev/null 2>&1
        END_NS=$(date +%s%N)
        LATENCY_MS=$(echo "scale=2; ($END_NS - $START_NS) / 1000000" | bc)
        echo "${PROMPT_NAME},${i},${LATENCY_MS}" >> "$HTTP_CSV"
        echo -n "  Run ${i}: ${LATENCY_MS}ms  "
    done
    echo ""
done
log "  HTTP results: $HTTP_CSV"

# =============================================================================
# STEP 4: B0 — DDS Single-Client (benchmark_final)
# =============================================================================
log ""
log "═══════════════════════════════════════════════════════"
log "STEP 4: B0 — DDS Single-Client Benchmark (benchmark_final)"
log "═══════════════════════════════════════════════════════"

B0_CSV="${RESULTS_DIR}/b0_dds_single_${TIMESTAMP}.csv"
"${BIN_DIR}/benchmark_final" "$NUM_RUNS" "$B0_CSV" "default" 2>&1 | tee -a "$LOG"
log "  B0 results: $B0_CSV"

# =============================================================================
# STEP 5: B1 — DDS Multi-Client (benchmark_multi_dds)
# =============================================================================
log ""
log "═══════════════════════════════════════════════════════"
log "STEP 5: B1 — DDS Multi-Client Benchmark"
log "═══════════════════════════════════════════════════════"

B1_DIR="${RESULTS_DIR}/b1_${TIMESTAMP}"
mkdir -p "$B1_DIR"

for NCLIENTS in 1 2 4; do
    log "  B1 with $NCLIENTS client(s)..."
    PIDS=()
    for C in $(seq 1 "$NCLIENTS"); do
        "${BIN_DIR}/benchmark_multi_dds" "$NUM_RUNS" "${B1_DIR}/client_${NCLIENTS}c_${C}.csv" "default" "$C" &
        PIDS+=($!)
    done

    # Wait for all clients
    for PID in "${PIDS[@]}"; do
        wait "$PID" 2>/dev/null || true
    done

    log "  Done: ${NCLIENTS} clients → ${B1_DIR}/"
done

# Aggregate B1 results
B1_AGG="${RESULTS_DIR}/b1_aggregated_${TIMESTAMP}.csv"
echo "num_clients,prompt_type,run,latency_ms" > "$B1_AGG"
for F in "${B1_DIR}"/*.csv; do
    NCLIENTS=$(basename "$F" | grep -o '[0-9]*c' | grep -o '[0-9]*')
    tail -n +2 "$F" >> "$B1_AGG" 2>/dev/null || true
done
log "  B1 aggregated: $B1_AGG"

# =============================================================================
# STEP 6: B2 — DDS Streaming (benchmark_stream_dds)
# =============================================================================
log ""
log "═══════════════════════════════════════════════════════"
log "STEP 6: B2 — DDS Streaming Benchmark (TTFT + ITL)"
log "═══════════════════════════════════════════════════════"

B2_CSV="${RESULTS_DIR}/b2_streaming_${TIMESTAMP}.csv"
"${BIN_DIR}/benchmark_stream_dds" "$NUM_RUNS" "$B2_CSV" "default" 2>&1 | tee -a "$LOG"
log "  B2 results: $B2_CSV"

# =============================================================================
# STEP 7: HTTP Streaming Baseline
# =============================================================================
log ""
log "═══════════════════════════════════════════════════════"
log "STEP 7: HTTP Streaming Baseline"
log "═══════════════════════════════════════════════════════"

HTTP_STREAM_CSV="${RESULTS_DIR}/http_streaming_${TIMESTAMP}.csv"
echo "prompt,run,ttft_ms,total_ms,num_chunks" > "$HTTP_STREAM_CSV"

PROMPTS=("Count to 5." "Explain machine learning." "Write about neural networks and backpropagation.")
PROMPT_NAMES=("simple" "medium" "complex")

for IDX in 0 1 2; do
    PROMPT="${PROMPTS[$IDX]}"
    PNAME="${PROMPT_NAMES[$IDX]}"
    log "  HTTP Stream [$PNAME]"

    for i in $(seq 1 "$NUM_RUNS"); do
        START_NS=$(date +%s%N)
        FIRST_CHUNK_NS=""
        CHUNK_COUNT=0

        # Use curl with --no-buffer to get SSE chunks as they arrive
        while IFS= read -r LINE; do
            if [[ "$LINE" == data:* ]]; then
                CHUNK_COUNT=$((CHUNK_COUNT + 1))
                if [[ -z "$FIRST_CHUNK_NS" ]]; then
                    FIRST_CHUNK_NS=$(date +%s%N)
                fi
            fi
        done < <(curl -sN -X POST "http://127.0.0.1:${PORT}/v1/chat/completions" \
            -H "Content-Type: application/json" \
            -d "{\"model\":\"default\",\"messages\":[{\"role\":\"user\",\"content\":\"${PROMPT}\"}],\"max_tokens\":100,\"stream\":true}" \
            2>/dev/null || true)

        END_NS=$(date +%s%N)

        if [[ -n "$FIRST_CHUNK_NS" ]]; then
            TTFT_MS=$(echo "scale=2; ($FIRST_CHUNK_NS - $START_NS) / 1000000" | bc)
            TOTAL_MS=$(echo "scale=2; ($END_NS - $START_NS) / 1000000" | bc)
        else
            TTFT_MS="-1"
            TOTAL_MS=$(echo "scale=2; ($END_NS - $START_NS) / 1000000" | bc)
        fi

        echo "${PNAME},${i},${TTFT_MS},${TOTAL_MS},${CHUNK_COUNT}" >> "$HTTP_STREAM_CSV"
        echo -n "  Run ${i}: TTFT=${TTFT_MS}ms total=${TOTAL_MS}ms chunks=${CHUNK_COUNT}  "
    done
    echo ""
done
log "  HTTP Stream results: $HTTP_STREAM_CSV"

# =============================================================================
# STEP 8: Summary
# =============================================================================
log ""
log "═══════════════════════════════════════════════════════"
log "SUMMARY"
log "═══════════════════════════════════════════════════════"
log "Timestamp:     $TIMESTAMP"
log "Model:         $MODEL"
log "Runs per test: $NUM_RUNS"
log ""
log "Results:"
log "  HTTP Baseline:  $HTTP_CSV"
log "  B0 DDS Single:  $B0_CSV"
log "  B1 DDS Multi:   $B1_DIR/"
log "  B1 Aggregated:  $B1_AGG"
log "  B2 DDS Stream:  $B2_CSV"
log "  HTTP Streaming:  $HTTP_STREAM_CSV"
log "  Server Log:     ${RESULTS_DIR}/server_${TIMESTAMP}.log"
log "  Benchmark Log:  $LOG"
log ""

# Quick comparison: show B0 CSV vs HTTP CSV
log "─── B0 DDS Results ───"
cat "$B0_CSV" 2>/dev/null | tee -a "$LOG"
log ""
log "─── HTTP Baseline Results ───"
cat "$HTTP_CSV" 2>/dev/null | tee -a "$LOG"
log ""
log "─── B2 Streaming Results ───"
cat "$B2_CSV" 2>/dev/null | tee -a "$LOG"
log ""
log "─── HTTP Streaming Results ───"
cat "$HTTP_STREAM_CSV" 2>/dev/null | tee -a "$LOG"

log ""
log "Benchmark complete! Check $RESULTS_DIR for all files."

# Cleanup
kill_server
