#!/bin/bash

# Must run this as root
[[ $EUID -ne 0 ]] && \
    echo "Please run me as root." && \
    exit 1

# Load common script
source $(readlink -f $(dirname $0))/common.sh

# Show help
if [[ ! -z $(get_flag help) ]]; then
    echo "Starts LGEN for ruleset mapping, locality, and" \
         "a custom inter packet gap in ms."
    echo "Usage: $0 --ruleset RULESET --locality LOCALITY --ipg IPG" \
         "--signal PID"
    echo "--ruleset: Ruleset file to load. Name only."
    echo "--locality: Locality file to load. Name only."
    echo "--ipg: Inter-packet gap file to load. Name only."
    echo "--signal: Signal PID on every event change."
    exit 1
fi

ruleset=$(get_flag ruleset)
locality=$(get_flag locality)
ipg=$(get_flag ipg)
signal=$(get_flag signal)

[[ -z $ruleset ]] && echo "Argument --ruleset is missing" && exit 1
[[ -z $locality ]] && echo "Argument --locality is missing" && exit 1
[[ -z $ipg ]] && echo "Argument --ipg is missing" && exit 1
[[ -z $signal ]] && signal=0

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

cmd="$base_dir/simple-packet-gen/run.sh --tx $txport --rx $rxport \
--eal \"-w $pci_rx -w $pci_tx\" --p-mapping \
--file1 $generated_dir/$ruleset/mapping.txt \
--file2 $timestamp_dir/$ipg \
--file3 $locality_dir/$locality \
--n1 0 --rxq 4 --txq $lgen_txq --signal $signal --time-limit 120"

echo "$cmd"
eval "$cmd"
