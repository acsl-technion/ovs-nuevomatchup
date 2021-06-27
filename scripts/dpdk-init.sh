#!/bin/bash

# Must run this as root
[[ $EUID -ne 0 ]] && \
    echo "Please run me as root." && \
    exit 1

# Load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh

devbind=$base_dir/dpdk/usertools/dpdk-devbind.py

echo "Loading uio and igb_uio modules"
modprobe uio
modprobe igb_uio

echo "Binding $pci_tx and $pci_rx to igb_uio"
$devbind -b igb_uio $pci_tx $pci_rx

