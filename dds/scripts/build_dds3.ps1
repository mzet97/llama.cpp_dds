# Build only llama-server
wsl -d Ubuntu -e bash -c "cd /mnt/e/TI/git/llama.cpp_dds/build && make llama-server 2>&1 | tail -80"
