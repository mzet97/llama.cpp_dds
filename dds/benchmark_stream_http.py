#!/usr/bin/env python3
"""
B2: Streaming HTTP Benchmark — TTFT & Inter-Token Latency

Sends requests with "stream": true and measures:
  - TTFT  = time from request send to first SSE data chunk
  - ITL   = inter-token latency between successive chunks
  - Total = time from request send to [DONE] sentinel

Usage: python benchmark_stream_http.py <num_runs> <csv_file> [model] [host] [port]
"""

import csv
import http.client
import json
import statistics
import sys
import time


DEFAULT_MODEL = "tinyllama"
DEFAULT_HOST  = "127.0.0.1"
DEFAULT_PORT  = 8080

PROMPTS = [
    ("complex",
     "Write a detailed technical explanation of how neural networks work, including backpropagation, gradient "
     "descent, and the role of activation functions."),
    ("simple", "What is 2+2?"),
]


def _send_stream(host: str, port: int, prompt: str, model: str):
    """Returns dict with ttft_ms, itl list, total_ms, num_chunks."""
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.3,
        "max_tokens": 100,
        "stream": True,
    })

    # Create a fresh connection per streaming request to avoid BrokenPipe
    # after the server closes the chunked transfer.
    conn = http.client.HTTPConnection(host, port, timeout=120)
    t_start = time.perf_counter()
    conn.request("POST", "/v1/chat/completions", body=body,
                 headers={"Content-Type": "application/json"})
    resp = conn.getresponse()

    if resp.status != 200:
        _ = resp.read()
        conn.close()
        return {"ttft_ms": -1, "itl": [], "total_ms": -1, "num_chunks": 0}

    ttft_ms     = None
    itl         = []
    num_chunks  = 0
    t_prev      = t_start
    _conn_ref   = conn  # keep reference for close

    # Read SSE stream line-by-line
    while True:
        line = resp.readline()
        if not line:
            break
        line = line.decode("utf-8", errors="replace").strip()

        if not line.startswith("data: "):
            continue

        payload = line[6:]  # strip "data: "

        if payload == "[DONE]":
            break

        t_now = time.perf_counter()
        try:
            chunk = json.loads(payload)
        except json.JSONDecodeError:
            continue

        # Check if chunk has actual content
        choices = chunk.get("choices", [])
        if not choices:
            continue
        delta = choices[0].get("delta", {})
        content = delta.get("content", "")
        if not content:
            continue

        num_chunks += 1
        if ttft_ms is None:
            ttft_ms = (t_now - t_start) * 1000.0
        else:
            itl.append((t_now - t_prev) * 1000.0)
        t_prev = t_now

    total_ms = (time.perf_counter() - t_start) * 1000.0
    _conn_ref.close()

    return {
        "ttft_ms":    ttft_ms if ttft_ms is not None else total_ms,
        "itl":        itl,
        "total_ms":   total_ms,
        "num_chunks": num_chunks,
    }


def _percentile(data, pct):
    if not data:
        return 0.0
    s = sorted(data)
    idx = int(len(s) * pct)
    return s[min(idx, len(s) - 1)]


def main():
    num_runs = int(sys.argv[1]) if len(sys.argv) > 1 else 64  # N=64 for statistical significance per Cohen (1988)
    csv_path = sys.argv[2]      if len(sys.argv) > 2 else None
    model    = sys.argv[3]      if len(sys.argv) > 3 else DEFAULT_MODEL
    host     = sys.argv[4]      if len(sys.argv) > 4 else DEFAULT_HOST
    port     = int(sys.argv[5]) if len(sys.argv) > 5 else DEFAULT_PORT

    csv_file = None
    writer   = None
    if csv_path:
        csv_file = open(csv_path, "w", newline="")
        writer = csv.writer(csv_file)
        writer.writerow(["prompt_type", "iteration", "ttft_ms", "itl_mean_ms",
                          "itl_p50_ms", "itl_p95_ms", "total_ms", "num_chunks"])

    for pname, prompt in PROMPTS:
        print(f"\n--- Streaming HTTP: {pname} ---")

        # warmup
        for _ in range(2):
            _send_stream(host, port, prompt, model)

        ttfts   = []
        totals  = []
        all_itl = []

        for i in range(num_runs):
            r = _send_stream(host, port, prompt, model)
            ttfts.append(r["ttft_ms"])
            totals.append(r["total_ms"])
            all_itl.extend(r["itl"])

            if writer:
                itl = r["itl"]
                writer.writerow([
                    pname, i,
                    f'{r["ttft_ms"]:.3f}',
                    f'{statistics.mean(itl) if itl else 0:.3f}',
                    f'{_percentile(itl, 0.50):.3f}',
                    f'{_percentile(itl, 0.95):.3f}',
                    f'{r["total_ms"]:.3f}',
                    r["num_chunks"],
                ])

        # Summary
        def _stats(v):
            return (f"mean={statistics.mean(v):.2f} "
                    f"p50={_percentile(v, 0.50):.2f} "
                    f"p95={_percentile(v, 0.95):.2f} "
                    f"std={statistics.stdev(v) if len(v) > 1 else 0:.2f}")

        print(f"  TTFT  {_stats(ttfts)} ms")
        if all_itl:
            print(f"  ITL   {_stats(all_itl)} ms")
        print(f"  Total {_stats(totals)} ms")

    if csv_file:
        csv_file.close()


if __name__ == "__main__":
    main()
