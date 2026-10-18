#!/bin/bash
# Compile and run benchmark with SHM config
export CYCLONEDDS_URI=file:///mnt/e/TI/git/llama.cpp_dds/dds/cyclonedds-shm.xml

cd /mnt/e/TI/git/llama.cpp_dds/dds

echo "=== Compiling persistent client ==="
g++ -std=c++17 -c benchmark_persistent.cpp -o benchmark_persistent.o \
    -I/mnt/e/TI/git/llama.cpp_dds/dds/idl \
    -I/home/zet/cyclonedds/install/include

g++ benchmark_persistent.o idl/LlamaDDS.o -o benchmark_persistent \
    -L/home/zet/cyclonedds/install/lib \
    -lddsc -lddscxx -pthread \
    -Wl,-rpath,/home/zet/cyclonedds/install/lib

echo "=== Running benchmark ==="
./benchmark_persistent -n 5
