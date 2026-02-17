# Install CycloneDDS C++ bindings
wsl -d Ubuntu -e bash -c "cd /home/zet && git clone --depth 1 https://github.com/eclipse-cyclonedds/cyclonedds-cxx.git && cd cyclonedds-cxx && mkdir build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=/home/zet/cyclonedds/install && make -j\$(nproc) && make install"
