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
NUM_TESTS = 10

PROMPTS = [
    {"name": "simple", "prompt": "What is 2+2?"},
    {"name": "medium", "prompt": "Explain machine learning in a few sentences."},
    {"name": "complex", "prompt": "Write a detailed technical explanation of how neural networks work, including backpropagation, gradient descent, and the role of activation functions."}
]

def run_test(conn, prompt, num_tests):
    latencies = []
    
    payload = json.dumps({
        "model": "phi4-mini", # Adjust model name if needed
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": 30, # Match benchmark_final.cpp
        "temperature": 0.3,
        "stream": False
    })
    headers = {"Content-Type": "application/json"}

    for i in range(num_tests):
        try:
            start_time = time.perf_counter()
            
            conn.request("POST", "/v1/chat/completions", body=payload, headers=headers)
            response = conn.getresponse()
            data = response.read() # Read fully
            
            end_time = time.perf_counter()
            
            if response.status == 200:
                latencies.append((end_time - start_time) * 1000) # ms
            else:
                print(f"Error: {response.status}")
                latencies.append(-1)
                
        except Exception as e:
            print(f"Exception: {e}")
            latencies.append(-1)
            # Reconnect on error
            conn.close()
            conn.connect()
            
        time.sleep(0.1) # 100ms pause like C++ benchmark

    return latencies

def main():
    print("=== HTTP Persistent Benchmark ===")
    print(f"Tests per prompt: {NUM_TESTS}")
    print(f"Target: {HOST}:{PORT}")
    
    csv_file = None
    if len(sys.argv) > 2:
        csv_file = open(sys.argv[2], "w")
        csv_file.write("prompt_type,mean,std,p50,p95,p99\n")

    conn = http.client.HTTPConnection(HOST, PORT, timeout=60)
    
    try:
        conn.connect()
    except Exception as e:
        print(f"Failed to connect: {e}")
        return 1

    print("Connected. Running tests...")
    time.sleep(2)

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
        p50 = valid[int(len(valid) * 0.50)]
        p95 = valid[int(len(valid) * 0.95)]
        p99 = valid[int(len(valid) * 0.99)]
        
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
    if len(sys.argv) > 1:
        NUM_TESTS = int(sys.argv[1])
    sys.exit(main())
