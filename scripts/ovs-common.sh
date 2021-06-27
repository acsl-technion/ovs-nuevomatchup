#!/bin/bash

# Set OVS parameters
ovs_log_file="/usr/local/var/log/openvswitch/ovs-vswitchd.log"
ovs_db_sock_file="/usr/local/var/run/openvswitch/db.sock"
ovs_scripts_dir="/usr/local/share/openvswitch/scripts"
ovs_br=br1
export PATH="$PATH:$ovs_scripts_dir"

# OVS aliases
ovs_ctl=$(which ovs-ctl)
ovs_vsctl=$(which ovs-vsctl)
ovsdb_tool=$(which ovsdb-tool)
ovsdb_server=$(which ovsdb-server)
ovs_vswitchd=$(which ovs-vswitchd)
ovs_ofctl=$(which ovs-ofctl)
ovs_appctl=$(which ovs-appctl)

# Default arguments
ruleset=$(get_flag ruleset)
rate=0
interval=1000
initial_delay=0
initial_rules=0
do_not_delete=false

# Load arguments
[[ ! -z $(get_flag rate) ]] && rate=$(get_flag rate)
[[ ! -z $(get_flag interval) ]] && interval=$(get_flag interval)
[[ ! -z $(get_flag initial-delay) ]] && initial_delay=$(get_flag initial-delay)
[[ ! -z $(get_flag initial-rules) ]] && initial_rules=$(get_flag initial-rules)
[[ ! -z $(get_flag do-not-delete) ]] && do_not_delete=true

# Show help
function ovs_load_rules_help() {
    echo "--ruleset: The name of the ruleset to load"
    echo "--rate VAL: Load VAL OpenFlow rules per 'interval' seconds," \
         " delete old ones."
    echo "--interval VAL: Number of ms (best effort) to wait between adjacent"\
         "OpenFlow rule update command. default is 1000 ms."
    echo "--initial-delay VAL: Initial delay defore sending the first update" \
         "command. Default is 0 sec."
    echo "--initial-rules VAL: Used with --rate option, how many"      \
         "OpenFlow rules to load at the begining of the experimnet."
    echo "--do-not-delete: used with --rate option, do not delete old rules."
}

# Sample the current timestamp in nanoseconds. Returns a string 
# representation on the number of seconds since last print.
function timer_sample() {
    local timer_current_n=$(date +"%s%3N")
    (( timer_val=$timer_current_n-$timer_current ))
    if (( $timer_val < $interval )); then
        (( timer_diff=$interval-$timer_val ))
    else
        timer_diff=0
    fi
    (( timer_counter+=1 ))
    (( timer_total=$timer_total+$timer_diff ))
    (( timer_sleep=$timer_total/$timer_counter ))
    timer_current=$timer_current_n
}

# Reset time statistics for "timer_sleep" and "timer_sample"
function timer_reset() {
    timer_current=$(date +"%s%3N")
    timer_counter=0
    timer_total=0
}

# Make sure to sleep exacly the required amount of seconds between
# each two function invocations
function timer_sleep() {
    sleep $(echo $timer_sleep/1000 | bc -l)
}

# Load rules in rate experiments
function ovs_load_rules_with_rate() {
    echo "*** Rate experiment - using $rate flows from $ruleset" \
         "initial delay of $initial_delay sec" | \
         tee -a $ovs_log_file
    echo "*** Rate experiment - waiting for LGEN to start..." | \
         tee -a $ovs_log_file
    while [[ $(tail $ovs_log_file | grep "Extended stats" | wc -l) -eq 0 ]]
    do
        sleep 0.3
    done
    echo "*** Rate experiment - start"
    timer_reset
    
    sleep $initial_delay
    for (( i=0; i<$total; i+=$rate )); do
        timer_sample
        echo "*** Rate experiment - iteration, $timer_val ms since last "

        # After each update, delete old rules
        if [[ $do_not_delete == false ]]; then
            # Insert the same rules as before
            echo "$add_rules" | head -n $rate |    \
            $ovs_ofctl --bundle add-flow $ovs_br - \
            &>/dev/null

            timer_sleep

            # Delete all old rules
            echo "$del_rules" | head -n $rate |     \
            $ovs_ofctl --bundle del-flows $ovs_br - \
            &> /dev/null

        else
            # In case we do not delete rules, insert new chunk each iteration
            echo "$add_rules" | tail -n +$i | head -n $rate | \
            $ovs_ofctl --bundle add-flow $ovs_br -            \
            &> /dev/null

            timer_sleep
        fi
    done
    echo "*** Rate experiment - stop"
}

# Wait until $1 rules are loaded into OVS
function ovs_load_rules_wait() {
    r=0
    while (( r< $1 )); do
        [[ ! -e $ovs_log_file ]] && break
        r=$(tail $ovs_log_file | grep flow_mods | awk '{x+=$2}END{print x}')
    done
}

# This function loads the rules in the background
# Loads rules with rate "rate".
function ovs_load_rules() {
    # Delete all current OpenFlow rules in switch
    $ovs_ofctl --bundle del-flows $ovs_br

    # Reverse the flows as they are
    # ordered from most specifict to least specific
    flows=$(tac $ovs_flows)
    total=$(echo "$flows" | wc -l)

    # In case we do not update rules on the fly
    if [[ $rate -eq 0 && $initial_rules -eq 0 ]]; then
        echo "*** Loading OF rules from $ruleset (total $total OF rules)" | \
        tee -a $ovs_log_file
        echo "$flows" | $ovs_ofctl --bundle add-flow $ovs_br -
        ovs_load_rules_wait $total
        return
    fi

    [[ $initial_rules -eq 0 ]] && initial_rules=10
    bse_flows=$(echo "$flows" | head -n $initial_rules)
    add_rules=$(echo "$flows" | tail -n +$(( $initial_rules+1 )))
    del_rules=$(echo "$add_rules" | sed 's/add//g;s/, prio.*//g') 
    (( total-=$initial_rules ))

    echo "*** Loading $initial_rules rules..." | tee -a $ovs_log_file
    echo "$bse_flows" | $ovs_ofctl --bundle add-flow $ovs_br -
    ovs_load_rules_wait $initial_rules

    ovs_load_rules_with_rate
}


