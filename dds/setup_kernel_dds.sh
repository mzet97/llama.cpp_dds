#!/bin/bash
# Kernel tuning for CycloneDDS performance
# Run with: sudo bash setup_kernel_dds.sh
# Makes settings persistent in /etc/sysctl.d/90-dds-tuning.conf

echo "Applying DDS kernel tuning..."

# Increase UDP receive/send buffers to 8MB (default ~208KB)
sysctl -w net.core.rmem_max=8388608
sysctl -w net.core.rmem_default=2097152
sysctl -w net.core.wmem_max=8388608
sysctl -w net.core.wmem_default=2097152

# Network device backlog for bursty traffic
sysctl -w net.core.netdev_budget=600
sysctl -w net.core.netdev_max_backlog=2000

# IP fragmentation tuning
sysctl -w net.ipv4.ipfrag_time=3
sysctl -w net.ipv4.ipfrag_high_thresh=134217728

# Make persistent
cat > /etc/sysctl.d/90-dds-tuning.conf << 'EOF'
net.core.rmem_max=8388608
net.core.rmem_default=2097152
net.core.wmem_max=8388608
net.core.wmem_default=2097152
net.core.netdev_budget=600
net.core.netdev_max_backlog=2000
net.ipv4.ipfrag_time=3
net.ipv4.ipfrag_high_thresh=134217728
EOF

echo "DDS kernel tuning applied and persisted."
