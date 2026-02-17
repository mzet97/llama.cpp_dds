# Generate C++ types from IDL
wsl -d Ubuntu -e bash -c "cd /mnt/e/TI/git/llama.cpp_dds/dds/idl && /home/zet/cyclonedds/build/bin/idlc LlamaDDS.idl 2>&1"
