#!/usr/bin/env python3
"""
B1: Multi-Client HTTP Benchmark

Each process is a single HTTP client that sends N sequential requests and
writes per-request latencies to a CSV.  The orchestration script launches
multiple instances in parallel and aggregates afterwards.

Usage: python benchmark_multi_http.py <num_runs> <csv_file> [model] [client_id] [host] [port]
"""

import csv
import http.client
import json
import sys
import time

DEFAULT_MODEL = "tinyllama"
DEFAULT_HOST  = "127.0.0.1"
DEFAULT_PORT  = 8080

PROMPTS = [
    ("simple",  "What is 2+2?"),
    ("complex",
     "Write a detailed technical explanation of how neural networks work, including backpropagation, gradient "
     "descent, and the role of activation functions."),
]


def _send_one(conn: http.client.HTTPConnection, prompt: str, model: str) -> float:
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.3,
        "max_tokens": 30,
        "stream": False,
    })
    start = time.perf_counter()
    conn.request("POST", "/v1/chat/completions", body=body,
                 headers={"Content-Type": "application/json"})
    resp = conn.getresponse()
    _ = resp.read()
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    if resp.status != 200:
        return -1.0
    return elapsed_ms


def main():
    num_runs  = int(sys.argv[1]) if len(sys.argv) > 1 else 64  # N=64 for statistical significance per Cohen (1988)
    csv_path  = sys.argv[2]      if len(sys.argv) > 2 else None
    model     = sys.argv[3]      if len(sys.argv) > 3 else DEFAULT_MODEL
    client_id = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    host      = sys.argv[5]      if len(sys.argv) > 5 else DEFAULT_HOST
    port      = int(sys.argv[6]) if len(sys.argv) > 6 else DEFAULT_PORT

    conn = http.client.HTTPConnection(host, port, timeout=120)

    csv_file = None
    writer   = None
    if csv_path:
        csv_file = open(csv_path, "w", newline="")
        writer = csv.writer(csv_file)
        writer.writerow(["client_id", "prompt_type", "iteration", "latency_ms"])

    wall_start = time.monotonic()

    for pname, prompt in PROMPTS:
        # warmup: 2 discarded
        for _ in range(2):
            _send_one(conn, prompt, model)

        for i in range(num_runs):
            ms = _send_one(conn, prompt, model)
            if writer:
                writer.writerow([client_id, pname, i, f"{ms:.3f}"])

    wall_end = time.monotonic()
    wall_s   = wall_end - wall_start

    print(f"[C{client_id}] done in {wall_s:.2f}s  ({num_runs} runs/prompt)")

    if csv_file:
        csv_file.close()
    conn.close()


if __name__ == "__main__":
    main()
