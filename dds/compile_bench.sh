#!/bin/bash
cd /mnt/e/TI/git/llama.cpp_dds/dds

g++ -std=c++17 -c benchmark_client.cpp -o benchmark_client.o -I/home/zet/llama.cpp_dds/dds/idl -I/home/zet/cyclonedds/install/include

g++ benchmark_client.o idl/LlamaDDS.o -o benchmark_client -L/home/zet/cyclonedds/install/lib -lddsc -lddscxx -pthread -Wl,-rpath,/home/zet/cyclonedds/install/lib
