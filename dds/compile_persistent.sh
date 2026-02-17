#!/bin/bash
# Compile persistent benchmark client

cd /mnt/e/TI/git/llama.cpp_dds/dds

echo "Compiling benchmark_persistent.cpp..."

g++ -std=c++17 -c benchmark_persistent.cpp -o benchmark_persistent.o \
    -I/home/zet/llama.cpp_dds/dds/idl \
    -I/home/zet/cyclonedds/install/include

g++ benchmark_persistent.o idl/LlamaDDS.o -o benchmark_persistent \
    -L/home/zet/cyclonedds/install/lib \
    -lddsc -lddscxx -pthread \
    -Wl,-rpath,/home/zet/cyclonedds/install/lib

if [ $? -eq 0 ]; then
    echo "SUCCESS: benchmark_persistent compiled"
    ls -la benchmark_persistent
else
    echo "ERROR: compilation failed"
fi
