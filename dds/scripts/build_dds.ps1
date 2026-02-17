# Build llama.cpp with DDS support
wsl -d Ubuntu -e bash -c "cd /mnt/e/TI/git/llama.cpp_dds && mkdir -p build && cd build && cmake .. -DLLAMA_DDS=ON -DCMAKE_PREFIX_PATH=/home/zet/cyclonedds/install 2>&1 | tail -40"
