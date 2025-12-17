#!/bin/bash
set -x
set -e

if [ $# != 1 ]; then
    echo "[ERROR] Wrong arguments number. Abort."
    exit 0
fi

typeset num_vms=$1

sudo ip link add br0 type bridge
sudo ip link set br0 up
sudo ip addr add 192.168.100.1/24 dev br0

for ((i = 0 ; i < $num_vms ; i++ ));
do
    sudo ip tuntap add tap$i mode tap
    sudo ip link set tap$i up
    sudo ip link set tap$i master br0
done