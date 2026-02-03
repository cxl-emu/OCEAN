#!/bin/bash
set -x
set -e

git submodule add https://github.com/CXLMemUring/qemu lib/qemu
git submodule add https://github.com/CXLMemUring/tigon workloads/tigon

sudo apt update && sudo apt install llvm-dev clang libbpf-dev libclang-dev python3-pip libcxxopts-dev libboost-dev nvidia-cuda-dev libfmt-dev libspdlog-dev librdmacm-dev
pip install tomli
pip install gdown
sudo apt-get install libglib2.0-dev libgcrypt20-dev zlib1g-dev \
    autoconf automake libtool bison flex libpixman-1-dev bc \
    make ninja-build libncurses-dev libelf-dev libssl-dev debootstrap \
    libcap-ng-dev libattr1-dev libslirp-dev libslirp0 libpmem-dev

sudo apt update
sudo apt install software-properties-common
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update
sudo apt install gcc-13 g++-13

mkdir temp && cd temp
wget https://github.com/Kitware/CMake/releases/download/v4.2.3/cmake-4.2.3.tar.gz
tar zxvf cmake-4.2.3.tar.gz 
cd cmake-4.2.3/
sudo ./bootstrap
sudo make -j$(nproc)
sudo make install
cmake --version
cd ../..
sudo rm -rf temp

cd ./lib/qemu
mkdir -p build
cd build
../configure --prefix=/usr/local --target-list=x86_64-softmmu --enable-debug --enable-libpmem --enable-slirp
make -j$(nproc)
sudo make install
/usr/local/bin/qemu-system-x86_64 --version
