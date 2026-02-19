#!/usr/bin/env python3
"""
HTTP Benchmark for llama.cpp DDS comparison
"""

import time
import json
import sys
import http.client
import statistics
import math

HOST = "127.0.0.1"
PORT = 8080
NUM_TESTS = 10  # overridden by argv[1]

MODEL = "tinyllama"  # must match the model loaded by the server

PROMPTS = [
    {"name": "simple", "prompt": "What is 2+2?"},
    {"name": "medium", "prompt": "Explain machine learning in a few sentences."},
    {"name": "complex", "prompt": "Write a detailed technical explanation of how neural networks work, including backpropagation, gradient descent, and the role of activation functions."}
]

def _send_one(conn, payload, headers):
    """Send one request and return latency in ms, or -1 on error."""
    try:
        start_time = time.perf_counter()
        conn.request("POST", "/v1/chat/completions", body=payload, headers=headers)
        response = conn.getresponse()
        response.read()
        end_time = time.perf_counter()
        return (end_time - start_time) * 1000 if response.status == 200 else -1
    except Exception as e:
        print(f"  warmup error: {e}")
        try:
            conn.close()
            conn.connect()
        except Exception:
            pass
        return -1


def run_test(conn, prompt, num_tests):
    latencies = []

    payload = json.dumps({
        "model": MODEL,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": 30,
        "temperature": 0.3,
        "stream": False
    })
    headers = {"Content-Type": "application/json"}

    # Warmup: 2 discarded runs to prime KV-cache, page faults and thread pools.
    for _ in range(2):
        _send_one(conn, payload, headers)
        time.sleep(0.1)

    for i in range(num_tests):
        try:
            start_time = time.perf_counter()

            conn.request("POST", "/v1/chat/completions", body=payload, headers=headers)
            response = conn.getresponse()
            data = response.read()  # Read fully

            end_time = time.perf_counter()

            if response.status == 200:
                latencies.append((end_time - start_time) * 1000)  # ms
            else:
                print(f"Error: {response.status}")
                latencies.append(-1)

        except Exception as e:
            print(f"Exception: {e}")
            latencies.append(-1)
            # Reconnect on error
            try:
                conn.close()
                conn.connect()
            except Exception:
                pass

        time.sleep(0) # no artificial delay — measure back-to-back latency

    return latencies

def main():
    global NUM_TESTS, HOST, PORT, MODEL
    if len(sys.argv) > 1:
        NUM_TESTS = int(sys.argv[1])

    print("=== HTTP Persistent Benchmark ===")
    print(f"Tests per prompt: {NUM_TESTS}")

    csv_file = None
    if len(sys.argv) > 2:
        csv_file = open(sys.argv[2], "w")
        csv_file.write("prompt_type,mean,std,p50,p95,p99\n")

    if len(sys.argv) > 3:
        MODEL = sys.argv[3]
    if len(sys.argv) > 4:
        HOST = sys.argv[4]
    if len(sys.argv) > 5:
        PORT = int(sys.argv[5])

    print(f"Target: {HOST}:{PORT}")
    print(f"Model: {MODEL}")

    conn = http.client.HTTPConnection(HOST, PORT, timeout=30)

    try:
        conn.connect()
    except Exception as e:
        print(f"Failed to connect: {e}")
        return 1

    print("Connected. Running tests...")

    for p in PROMPTS:
        print(f"\n--- {p['name']} ---")
        print(f"Prompt: {p['prompt']}")

        results = run_test(conn, p['prompt'], NUM_TESTS)

        valid = [t for t in results if t > 0]

        if not valid:
            print("No successful requests!")
            continue

        mean = statistics.mean(valid)
        stddev = statistics.stdev(valid) if len(valid) > 1 else 0

        valid.sort()
        n = len(valid)
        p50 = valid[min(int(n * 0.50), n - 1)]
        p95 = valid[min(int(n * 0.95), n - 1)]
        p99 = valid[min(int(n * 0.99), n - 1)]

        print(f"Mean: {mean:.2f} ms")
        print(f"Std: {stddev:.2f} ms")
        print(f"p50: {p50:.2f} ms")
        print(f"p95: {p95:.2f} ms")
        print(f"p99: {p99:.2f} ms")

        print(f"\nCSV: {p['name']},{mean:.2f},{stddev:.2f},{p50:.2f},{p95:.2f},{p99:.2f}")

        if csv_file:
            csv_file.write(f"{p['name']},{mean:.2f},{stddev:.2f},{p50:.2f},{p95:.2f},{p99:.2f}\n")

    conn.close()
    if csv_file:
        csv_file.close()
        print(f"Results saved to {sys.argv[2]}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
