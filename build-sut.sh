#!/bin/bash

my_dir=$(readlink -f $PWD)
ovs_dir=$my_dir/ovs
dpdk_dir=$my_dir/dpdk
nmu_dir=$my_dir/nuevomatchup

if [[ ! -e $dpdk_dir ]]; then
    echo "Cloning DPDK 19.11 into $dpdk_dir..."
    git clone https://github.com/DPDK/dpdk.git $dpdk_dir
    git -C $dpdk_dir checkout tags/v19.11
fi

if [[ ! -e $dpdk_dir/build ]]; then
    echo "Building DPDK..."
    make -C $dpdk_dir config T=x86_64-native-linux-gcc O=build
    make -C $dpdk_dir -j4 O=build
fi

if [[ ! -e $nmu_dir ]]; then
    echo "Cloning libnuevomatchup..."
    git clone git@github.com:alonrs/nuevomatchup.git
    git -C $nmu_dir checkout revision
    (cd $nmu_dir && ./build.sh)
    make -C $nmu_dir -j4
    sudo make -C $nmu_dir install
    ln -s $nmu_dir/libcommon/lib/simd.h
fi

# Set configuration
if [[ ! -e scripts/.config ]]; then
    echo "Reading information from dpdk-devbind..."
    ./dpdk/usertools/dpdk-devbind.py --status-dev net
    echo "==="

    read -p "Enter RX PCI bus (LGEN->SUT): " pci_rx
    read -p "Enter TC PCI bus (SUT->LGEN): " pci_tx
    echo "pci_rx=$pci_rx" >> scripts/.config
    echo "pci_tx=$pci_tx" >> scripts/.config
fi

if [[ ! -e $ovs_dir ]]; then
    echo "Cloning OVS 2.13.1 into $ovs_dir..."
    git clone https://github.com/openvswitch/ovs.git $ovs_dir
    git -C $ovs_dir checkout tags/v2.13.1
    git -C $ovs_dir apply ovs.patch
fi

for f in dpif-netdev-nmu.c dpif-netdev-nmu.h simd.h; do
    if [[ ! -e $ovs_dir/lib/$f ]]; then
        echo "Copying module $f into OVS..."
        ln -s $my_dir/$f $ovs_dir/lib/$f
    fi
done

if [[ ! -e $ovs_dir/config.status ]]; then
    echo "Configuring OVS..."
    (cd $ovs_dir &&
     ./boot.sh   &&
     ./configure CFLAGS="-mfma -mpopcnt -O2 -g" \
     --with-dpdk="$dpdk_dir/build")
fi

echo "Building and installing OVS..."
sudo make -C $ovs_dir install
 
