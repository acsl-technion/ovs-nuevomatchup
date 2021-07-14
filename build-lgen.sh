#!/bin/bash

my_dir=$(readlink -f $PWD)
pgen_dir=$my_dir/simple-packet-gen

# Download and make packet generator
if [[ ! -e $pgen_dir ]]; then
    echo "Cloning simple-packet-gen"
    git clone git@github.com:alonrs/simple-packet-gen.git $pgen_dir
    git checkout f6799102b05980b3c5b76f58b1f550d3aeae4e9f
    (cd $pgen_dir && ./build.sh)
    make -C $pgen_dir -j4
fi

# Set configuration
if [[ ! -e scripts/.config ]]; then
    read -p "Enter RX PCI bus (SUT->LGEN): " pci_rx
    read -p "Enter TX PCI bus (LGEN->SUT): " pci_tx
    read -p "Enter SUT IP address: " sut_ip
    echo "pci_rx=$pci_rx" >> scripts/.config
    echo "pci_tx=$pci_tx" >> scripts/.config
    echo "sut_ip=$sut_ip" >> scripts/.config
fi

