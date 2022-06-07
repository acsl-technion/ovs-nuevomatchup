#!/bin/bash

libnmu_version=1.0.5
date=2022/04
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
    zipname="libnuevomatchup-x86-64-linux-${libnmu_version}.zip"
    echo "Downloading libnuevomatchup..."
    wget "https://alonrashelbach.files.wordpress.com/$date/$zipname"
    unzip $zipname -d $nmu_dir
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
    git -C $ovs_dir apply $my_dir/ovs.patch
fi

for f in dpif-netdev-nmu.c dpif-netdev-nmu.h; do
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

# These require sudo
if [[ "$EUID" -ne 0 ]]; then
    echo "Cannot continue setup; please run me as root"
    exit 0
fi

echo "Installing libnuevomatchup..."
(cd $nmu_dir && sudo ./install.sh)

echo "Building and installing OVS..."
sudo make -C $ovs_dir install
 
