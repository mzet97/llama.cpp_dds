#!/bin/bash
cd /mnt/e/TI/git/llama.cpp_dds/dds/idl
gcc -std=c11 -c LlamaDDS.c -o LlamaDDS.o -I. -I/home/zet/cyclonedds/install/include
