#!/bin/bash
# Compile IDL generated code
cd /home/zet/llama.cpp_dds/dds/idl
gcc -std=c11 -c LlamaDDS.c -o LlamaDDS.o -I. -I/home/zet/cyclonedds/install/include

# Compile test client
cd /home/zet/llama.cpp_dds/dds
g++ -std=c++17 -c test_client.cpp -o test_client.o -I/home/zet/llama.cpp_dds/dds/idl -I/home/zet/cyclonedds/install/include

# Link
g++ test_client.o idl/LlamaDDS.o -o test_client -L/home/zet/cyclonedds/install/lib -lddsc -lddscxx -pthread -Wl,-rpath,/home/zet/cyclonedds/install/lib
