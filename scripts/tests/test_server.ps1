# Test the server with DDS enabled (without a model, just check initialization)
wsl -d Ubuntu -e bash -c "cd /mnt/e/TI/git/llama.cpp_dds/build && timeout 5 ./bin/llama-server --enable-dds --dds-domain 0 2>&1 || true"
