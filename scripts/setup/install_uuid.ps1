# Install uuid library
wsl -d Ubuntu -e bash -c "echo 'zet' | sudo -S apt-get install -y uuid-dev 2>&1 | tail -10"
