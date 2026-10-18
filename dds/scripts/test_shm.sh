#!/bin/bash
# Test DDS with SHM configuration
export CYCLONEDDS_URI=file:///mnt/e/TI/git/llama.cpp_dds/dds/cyclonedds-shm.xml

echo "=== Testing DDS with SHM ==="
echo "Config: $CYCLONEDDS_URI"

/mnt/e/TI/git/llama.cpp_dds/dds/test_client 0 "What is 2+2?"
