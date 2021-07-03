#!/bin/bash

my_dir=$(readlink -f $PWD)
pgen_dir=$my_dir/simple-packet-gen

# Download and make packet generator
if [[ ! -e $pgen_dir ]]; then
    echo "Cloning simple-packet-gen"
    git clone git@github.com:alonrs/simple-packet-gen.git $pgen_dir
    git checkout 170c4c76288cff7ff5ae950ab19db57f7b705f61
    (cd $pgen_dir && ./build.sh && ./bind.sh)
fi

# Set configuration
if [[ ! -e scripts/.config ]]; then
    read -p "Enter STUB IP address: "
    echo "stub_ip=$stub_ip" >> scripts/.config
fi

