#!/bin/bash
echo "=== DDS Test ==="
for i in 1 2 3; do
    echo "Test $i:"
    /mnt/e/TI/git/llama.cpp_dds/dds/test_client 0 "What is 2+2?" 2>&1 | grep -E "(Content:|Finish reason)"
    echo ""
done
