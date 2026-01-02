#!/bin/bash 
set -eux 
if [ $# != 1 ]; then
    echo "[ERROR] Wrong arguments number. Abort."
    exit 0
fi

typeset num_vms=$1

DEV=enp23s0f0np0 
BR=br0
VNI=100 
MCAST=239.1.1.1 
BR_IP_SUFFIX=$(hostname | grep -oE '[0-9]+$' || echo 1) # optional auto-index
# Or set manually: 
# BR_IP_SUFFIX=<1..4>  

# Clean up 
ip link del $BR 2>/dev/null || true 
ip link del vxlan$VNI 2>/dev/null || true 

# Create bridge 
ip link add $BR type bridge 
ip link set $BR up 

# Create multicast VXLAN (no remote attribute!) 
ip link add vxlan$VNI type vxlan id $VNI group $MCAST dev $DEV dstport 4789 ttl 10 
ip link set vxlan$VNI up 
ip link set vxlan$VNI master $BR  

# Assign overlay IP 
ip addr add 192.168.100.$BR_IP_SUFFIX/24 dev $BR 

# Optional: add local TAPs for QEMU 
for ((i = 0 ; i < $num_vms ; i++ ));
do    
    ip tuntap add tap$i mode tap 
    ip link set tap$i up 
    ip link set tap$i master $BR
done  

echo "Bridge $BR ready on host $(hostname)"