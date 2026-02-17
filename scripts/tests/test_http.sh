#!/bin/bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model": "phi4-mini", "messages": [{"role": "user", "content": "What is 2+2?"}], "max_tokens": 20, "temperature": 0.3}'
