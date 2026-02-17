#!/usr/bin/env python3
import urllib.request
import json

data = {
    "model": "phi4-mini",
    "messages": [{"role": "user", "content": "What is 2+2?"}],
    "max_tokens": 20,
    "temperature": 0.3
}

req = urllib.request.Request(
    "http://127.0.0.1:8080/v1/chat/completions",
    data=json.dumps(data).encode("utf-8"),
    headers={"Content-Type": "application/json"}
)

with urllib.request.urlopen(req) as response:
    result = json.loads(response.read().decode("utf-8"))
    print("Content:", result["choices"][0]["message"]["content"])
    print("Usage:", result.get("usage", {}))
