#!/bin/bash
cd /e/TI/git/llama.cpp_dds
./build/bin/llama-server --enable-dds --model /home/zet/models/Phi-4-mini-instruct-Q3_K_M-GGUF/phi4-mini-q3_k_m.gguf -c 2048 --port 8080
