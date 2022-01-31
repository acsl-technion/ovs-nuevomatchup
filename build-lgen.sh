#!/bin/bash

my_dir=$(readlink -f $PWD)
pgen_dir=$my_dir/simple-packet-gen
devbind=$pgen_dir/dpdk/usertools/dpdk-devbind.py

# Download and make packet generator
if [[ ! -e $pgen_dir ]]; then
    echo "Cloning simple-packet-gen"
    git clone git@github.com:alonrs/simple-packet-gen.git $pgen_dir
    git -C $pgen_dir checkout 1ba09a229ac025e15d7816e290ed76b60fcc24da
    (cd $pgen_dir && ./build.sh)
    make -C $pgen_dir -j4
fi

# Set configuration
if [[ ! -e scripts/.config ]]; then
    echo "Reading information from dpdk-devbind..."
    $devbind --status-dev net
    echo "==="

    read -p "Enter RX PCI bus (SUT->LGEN): " pci_rx
    read -p "Enter TX PCI bus (LGEN->SUT): " pci_tx
    read -p "Enter SUT IP address: " sut_ip
    read -p "Enter LGEN number of TXQs: " lgen_txq
    echo "pci_rx=$pci_rx" >> scripts/.config
    echo "pci_tx=$pci_tx" >> scripts/.config
    echo "sut_ip=$sut_ip" >> scripts/.config
    echo "lgen_txq=$lgen_txq" >> scripts/.config

    # Configure RX and TX ports
    pci_0=$(echo -e "$pci_rx\n$pci_tx" | sort | head -1)
    if [[ $pci_0 == $pci_rx ]]; then
        rxport=0
        txport=1
    else
        rxport=1
        txport=0
    fi
    echo "rxport=$rxport" >> scripts/.config
    echo "txport=$txport" >> scripts/.config
fi

# Bind interfaces
source scripts/.config
$pgen_dir/bind.sh $pci_rx $pci_tx
