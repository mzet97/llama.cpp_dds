#!/usr/bin/env python3
import urllib.request
import json
import time
import sys

def test_http(prompt, max_tokens):
    data = {
        "model": "phi4-mini",
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "temperature": 0.3
    }

    req = urllib.request.Request(
        "http://127.0.0.1:8080/v1/chat/completions",
        data=json.dumps(data).encode("utf-8"),
        headers={"Content-Type": "application/json"}
    )

    start = time.time()
    with urllib.request.urlopen(req) as response:
        result = json.loads(response.read().decode("utf-8"))
        elapsed = time.time() - start

    content = result["choices"][0]["message"]["content"]
    usage = result.get("usage", {})
    return content, elapsed, usage

if __name__ == "__main__":
    prompt = "What is 2+2?"
    max_tokens = 30

    print("=== HTTP Test ===")
    for i in range(3):
        content, elapsed, usage = test_http(prompt, max_tokens)
        print(f"Test {i+1}: {elapsed:.2f}s | prompt={usage.get('prompt_tokens',0)} comp={usage.get('completion_tokens',0)}")
        print(f"  Content: {content[:60]}...")
