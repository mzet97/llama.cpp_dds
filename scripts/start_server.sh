#!/bin/bash
cd /e/TI/git/llama.cpp_dds
./build/bin/llama-server --enable-dds --model /home/zet/models/Qwen3.5-0.8B-Q3_K_M-GGUF/qwen3.5-0.8b-q3_k_m.gguf -c 2048 --port 8080
