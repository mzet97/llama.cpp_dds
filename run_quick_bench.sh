#!/bin/bash
# Quick benchmark run script - starts server, runs B0/B1/B2, compares with HTTP
set -e

BASE="/mnt/e/TI/git/tese/llama.cpp_dds"
BIN="${BASE}/build-wsl/bin"
RESULTS="${BASE}/dds/results"
MODEL="/mnt/e/TI/git/tese/models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
PORT=8080
NUM_RUNS=${1:-10}
TS=$(date +%Y%m%d_%H%M%S)

export LD_LIBRARY_PATH="${BIN}:/home/zet/cyclonedds/install/lib:${LD_LIBRARY_PATH:-}"
export CYCLONEDDS_URI="file://${BASE}/dds/cyclonedds-local.xml"

mkdir -p "$RESULTS"

echo "============================================"
echo "DDS-LLM Benchmark Suite — $TS"
echo "Model: $MODEL"
echo "Runs:  $NUM_RUNS"
echo "============================================"

# Kill any old server
pkill -f "llama-server" 2>/dev/null || true
sleep 1

# Start server
echo ""
echo "[1] Starting llama-server with DDS + GPU..."
"${BIN}/llama-server" \
    --model "$MODEL" \
    --port "$PORT" \
    --ctx-size 512 \
    -ngl 99 \
    --enable-dds \
    > "${RESULTS}/server_${TS}.log" 2>&1 &
SPID=$!
echo "  PID: $SPID"

# Wait for health
echo "  Waiting for server..."
for i in $(seq 1 60); do
    if curl -s "http://127.0.0.1:${PORT}/health" 2>/dev/null | grep -q status; then
        echo "  Ready! (${i}s)"
        break
    fi
    if ! kill -0 "$SPID" 2>/dev/null; then
        echo "  ERROR: Server died. Log:"
        tail -20 "${RESULTS}/server_${TS}.log"
        exit 1
    fi
    sleep 1
done

# Warmup HTTP
echo ""
echo "[2] HTTP Warmup..."
for i in 1 2 3; do
    curl -s -X POST "http://127.0.0.1:${PORT}/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -d '{"model":"default","messages":[{"role":"user","content":"hi"}],"max_tokens":5}' \
        > /dev/null 2>&1 || true
done
echo "  Done"

# HTTP Baseline
echo ""
echo "[3] HTTP Baseline (non-streaming)..."
HTTP_CSV="${RESULTS}/http_baseline_${TS}.csv"
echo "prompt_type,run,latency_ms" > "$HTTP_CSV"

for PTYPE in simple medium complex; do
    case "$PTYPE" in
        simple)  P="What is 2+2?" ;;
        medium)  P="Explain machine learning in a few sentences." ;;
        complex) P="Write a detailed technical explanation of how neural networks work, including backpropagation." ;;
    esac
    echo "  $PTYPE:"
    for i in $(seq 1 $NUM_RUNS); do
        S=$(date +%s%N)
        curl -s -X POST "http://127.0.0.1:${PORT}/v1/chat/completions" \
            -H "Content-Type: application/json" \
            -d "{\"model\":\"default\",\"messages\":[{\"role\":\"user\",\"content\":\"${P}\"}],\"max_tokens\":30}" \
            > /dev/null 2>&1
        E=$(date +%s%N)
        MS=$(echo "scale=2; ($E - $S) / 1000000" | bc)
        echo -n "    ${MS}ms "
        echo "${PTYPE},${i},${MS}" >> "$HTTP_CSV"
    done
    echo ""
done

# B0: DDS Single-Client
echo ""
echo "[4] B0: DDS Single-Client (benchmark_final)..."
B0_CSV="${RESULTS}/b0_dds_${TS}.csv"
"${BIN}/benchmark_final" "$NUM_RUNS" "$B0_CSV" "default" 2>&1 | grep -E "Mean|Std|p50|p95|CSV|---"

# B2: DDS Streaming
echo ""
echo "[5] B2: DDS Streaming (benchmark_stream_dds)..."
B2_CSV="${RESULTS}/b2_streaming_${TS}.csv"
"${BIN}/benchmark_stream_dds" "$NUM_RUNS" "$B2_CSV" "default" 2>&1 | grep -E "TTFT|ITL|Total|chunks|---"

# B1: DDS Multi-Client
echo ""
echo "[6] B1: DDS Multi-Client..."
for NC in 1 2 4; do
    echo "  ${NC} client(s):"
    B1_CSV="${RESULTS}/b1_${NC}c_${TS}.csv"
    PIDS=()
    for C in $(seq 1 $NC); do
        "${BIN}/benchmark_multi_dds" "$NUM_RUNS" "${RESULTS}/b1_${NC}c_client${C}_${TS}.csv" "default" "$C" &
        PIDS+=($!)
    done
    for P in "${PIDS[@]}"; do
        wait "$P" 2>/dev/null || true
    done
    echo "  Done"
done

# HTTP Streaming Baseline
echo ""
echo "[7] HTTP Streaming Baseline..."
HTTP_S_CSV="${RESULTS}/http_stream_${TS}.csv"
echo "prompt,run,total_ms,num_chunks" > "$HTTP_S_CSV"

for PTYPE in simple medium complex; do
    case "$PTYPE" in
        simple)  P="Count to 5." ;;
        medium)  P="Explain machine learning." ;;
        complex) P="Write about neural networks and backpropagation." ;;
    esac
    echo "  $PTYPE:"
    for i in $(seq 1 $NUM_RUNS); do
        S=$(date +%s%N)
        RESP=$(curl -sN -X POST "http://127.0.0.1:${PORT}/v1/chat/completions" \
            -H "Content-Type: application/json" \
            -d "{\"model\":\"default\",\"messages\":[{\"role\":\"user\",\"content\":\"${P}\"}],\"max_tokens\":100,\"stream\":true}" \
            2>/dev/null || true)
        E=$(date +%s%N)
        MS=$(echo "scale=2; ($E - $S) / 1000000" | bc)
        CHUNKS=$(echo "$RESP" | grep -c "^data:" || echo 0)
        echo -n "    ${MS}ms(${CHUNKS}ch) "
        echo "${PTYPE},${i},${MS},${CHUNKS}" >> "$HTTP_S_CSV"
    done
    echo ""
done

# Summary
echo ""
echo "============================================"
echo "RESULTS SUMMARY"
echo "============================================"
echo ""
echo "--- HTTP Baseline ---"
cat "$HTTP_CSV"
echo ""
echo "--- B0 DDS Single ---"
cat "$B0_CSV" 2>/dev/null || echo "(see stdout above)"
echo ""
echo "--- B2 DDS Streaming ---"
cat "$B2_CSV" 2>/dev/null || echo "(see stdout above)"
echo ""
echo "--- HTTP Streaming ---"
cat "$HTTP_S_CSV"
echo ""
echo "All results in: $RESULTS"
echo ""

# Cleanup
kill "$SPID" 2>/dev/null || true
wait "$SPID" 2>/dev/null || true
echo "Server stopped. Done!"
