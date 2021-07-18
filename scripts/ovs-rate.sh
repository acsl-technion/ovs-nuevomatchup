#!/bin/bash

# Must run this as root
[[ $EUID -ne 0 ]] && \
    echo "Please run me as root." && \
    exit 1

# Load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh
source $scripts_dir/ovs-common.sh

if [[ ! -z $(get_flag help) ]]; then
    echo "Start rate experiment"
    echo "Usage: $0 --ruleset VALUE --rate VALUE [options]" 
    echo "--ruleset: The name of the ruleset to load"
    echo "--rate VAL: Load VAL OpenFlow rules per 'interval' seconds," \
         " delete old ones."
    echo "--interval VAL: Number of ms (best effort) to wait between adjacent"\
         "OpenFlow rule update command. default is 1000 ms."
    echo "--initial-delay VAL: Initial delay defore sending the first update" \
         "command. Default is 0 sec."
    echo "--initial-rules VAL: How many"      \
         "OpenFlow rules to load at the begining of the experimnet."
    echo "--do-not-delete: used with --rate option, do not delete old rules."
    exit 1
fi

# Default args
initial_delay=0
interval=1000
do_not_delete=false

ruleset=$(get_flag ruleset)
rate=$(get_flag rate)

# Load arguments
[[ -z $ruleset ]] && echo "Argument --ruleset is missing" && exit 1
[[ -z $rate ]] && echo "Argument --rate is missing" && exit 1
[[ ! -z $(get_flag interval) ]] && interval=$(get_flag interval)
[[ ! -z $(get_flag initial-delay) ]] && initial_delay=$(get_flag initial-delay)
[[ ! -z $(get_flag initial-rules) ]] && initial_rules=$(get_flag initial-rules)
[[ ! -z $(get_flag do-not-delete) ]] && do_not_delete=true

ovs_load_rules_update
