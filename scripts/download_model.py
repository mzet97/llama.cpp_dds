#!/usr/bin/env python3
from huggingface_hub import snapshot_download

print("Downloading model...")
snapshot_download(
    'cosmeq/Qwen3.5-0.8B-Q3_K_M-GGUF',
    local_dir='/home/zet/models/Qwen3.5-0.8B-Q3_K_M-GGUF'
)
print("Done!")
