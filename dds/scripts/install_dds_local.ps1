# Install CycloneDDS to local directory
wsl -d Ubuntu -e bash -c "cd /home/zet/cyclonedds && rm -rf build && mkdir build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=/home/zet/cyclonedds/install && make -j\$(nproc) && make install"
