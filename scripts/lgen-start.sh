#!/bin/bash

# Must run this as root
[[ $EUID -ne 0 ]] && \
    echo "Please run me as root." && \
    exit 1

# Load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh


# Show help
if [[ ! -z $(get_flag help) ]]; then
    echo "Starts LGEN for ruleset mapping, locality, and"
         "inter-packet gap "
    echo "Usage: $0 --ruleset RULESET --locality LOCALITY"
    echo "--ruleset: Ruleset file to load. Name only."
    echo "--locality: Locality file to load. Name only."
    echo "--ipg: Inter-packet gap file to load. Name only."
    exit 1
fi

ruleset=$(get_flag ruleset)
locality=$(get_flag locality)
ipg=$(get_flag ipg)

[[ -z $ruleset ]] && echo "Argument --ruleset is missing" && exit 1
[[ -z $locality ]] && echo "Argument --locality is missing" && exit 1
[[ -z $ipg ]] && echo "Argument --ipg is missing" && exit 1

# Check that files are valid
if [[ ! -d $generated_dir/$ruleset ]]; then
    echo "Cannot find ruleset \"$ruleset\" in \"$generated_dir\""
    exit 1
fi

if [[ ! -e $locality_dir/$locality ]]; then
    echo "Cannot find locality file \"$locality\" in \"$locality_dir\""
    exit 1
fi

if [[ ! -e $timestamp_dir/$ipg ]]; then
    echo "Cannot find IPG file \"$ipg\" in \"$timestamp_dir\""
    exit 1
fi

$base_dir/simple-packet-gen/run.sh --tx 1 --rx 0            \
    --eal "-w $pci_rx -w $pci_tx"                           \
    --p-mapping --file1 $generated_dir/$ruleset/mapping.txt \
    --file2 $timestamp_dir/$ipg                             \
    --file3 $locality_dir/$locality                         \
    --n1 5 --txq 5
