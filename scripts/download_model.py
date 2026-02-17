#!/usr/bin/env python3
from huggingface_hub import snapshot_download

print("Downloading model...")
snapshot_download(
    'cosmeq/Phi-4-mini-instruct-Q3_K_M-GGUF',
    local_dir='/home/zet/models/Phi-4-mini-instruct-Q3_K_M-GGUF'
)
print("Done!")
