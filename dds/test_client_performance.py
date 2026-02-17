#!/usr/bin/env python3
"""
HTTP Client Python - Test with Phi-4-mini model
Measures inference time for the DDS-enabled server
"""

import time
import json
import sys
import http.client

def main():
    # Server configuration
    host = "127.0.0.1"
    port = 8080

    prompt = "Some os 4 primeiros números deste ano (2026)."

    print("=" * 60)
    print("HTTP Client - Phi-4-mini Inference Test")
    print("=" * 60)
    print(f"Server: {host}:{port}")
    print(f"Prompt: {prompt}")
    print()

    # Prepare request
    payload = json.dumps({
        "model": "phi4-mini",
        "messages": [
            {"role": "user", "content": prompt}
        ],
        "max_tokens": 50,
        "temperature": 0.3,
        "stream": False
    })

    headers = {"Content-Type": "application/json"}

    # Connect and send request
    print("Sending request...")
    start_time = time.time()

    try:
        conn = http.client.HTTPConnection(host, port, timeout=60)
        conn.request("POST", "/v1/chat/completions", body=payload, headers=headers)
        response = conn.getresponse()

        end_time = time.time()
        elapsed = end_time - start_time

        print(f"Status: {response.status}")
        print(f"Time elapsed: {elapsed:.2f} seconds")
        print()

        if response.status == 200:
            data = response.read().decode()
            result = json.loads(data)

            if "choices" in result and len(result["choices"]) > 0:
                content = result["choices"][0]["message"]["content"]
                print("=" * 60)
                print("Model Response:")
                print("=" * 60)
                print(content)
                print("=" * 60)

                # Calculate expected result
                numbers = [2, 0, 2, 6]
                expected = sum(numbers)
                print(f"\nExpected sum (2+0+2+6): {expected}")

                # Show timing breakdown
                if "usage" in result:
                    usage = result["usage"]
                    print(f"\nToken usage:")
                    print(f"  Prompt tokens: {usage.get('prompt_tokens', 'N/A')}")
                    print(f"  Completion tokens: {usage.get('completion_tokens', 'N/A')}")
                    print(f"  Total tokens: {usage.get('total_tokens', 'N/A')}")

                print(f"\nTotal time: {elapsed:.2f}s")
        else:
            print(f"Error: {response.status} {response.reason}")
            print(response.read().decode())

        conn.close()

    except Exception as e:
        print(f"Error: {e}")
        return 1

    return 0

if __name__ == "__main__":
    sys.exit(main())
