#!/bin/bash
cd /mnt/e/TI/git/llama.cpp_dds/dds

echo "=== Compiling persistent client ==="
g++ -std=c++17 -c persistent_client.cpp -o persistent_client.o \
    -I/mnt/e/TI/git/llama.cpp_dds/dds/idl \
    -I/home/zet/cyclonedds/install/include

g++ persistent_client.o idl/LlamaDDS.o -o persistent_client \
    -L/home/zet/cyclonedds/install/lib \
    -lddsc -lddscxx -pthread \
    -Wl,-rpath,/home/zet/cyclonedds/install/lib

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

echo "=== Running persistent client (10 requests) ==="
./persistent_client 10 "What is 2+2?"
