# Build llama.cpp with DDS
wsl -d Ubuntu -e bash -c "cd /mnt/e/TI/git/llama.cpp_dds/build && make -j\$(nproc) 2>&1 | tail -100"
