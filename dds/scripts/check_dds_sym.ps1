# Check DDS symbols in the binary
wsl -d Ubuntu -e bash -c "nm /mnt/e/TI/git/llama.cpp_dds/build/bin/llama-server | grep -i dds | head -10"
