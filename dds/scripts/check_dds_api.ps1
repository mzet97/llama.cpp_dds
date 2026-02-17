# Check CycloneDDS headers
wsl -d Ubuntu -e bash -c "grep -r 'dds_strerror' /home/zet/cyclonedds/install/include/ | head -5"
