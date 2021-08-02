#!/bin/bash

# Must run this as root
[[ $EUID -ne 0 ]] && \
    echo "Please run me as root." && \
    exit 1

# Load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh
source $scripts_dir/ovs-common.sh

if [[ $# -le 2 ]]; then
    echo "Usage: $0 RULESET NAME [NAME..}"
    echo "Copies the last section within OVS log file to a dedicated file"
fi

ruleset="$1"
name="${@:2}"
name=$(echo "$name" | sed 's/ /-/g')

log=$generated_dir/$ruleset/$name
echo "Saving last OVS log to $log"
tac $ovs_log_file | sed '/nmu-cool-down-time/q' | tac > $log
chmod +r "$log"

