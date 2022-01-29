#!/bin/bash

# Load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh

if [[ $(get_flag help) -eq 1 ]]; then
    echo "Usage: $0 [--default] key=val [key=val [key=val ...]]"
    echo "Set NMU manual parameters."
    echo "--default: Set parameters to default value"
    echo "Valid keys:"
    echo " * nmu-cool-down-time-ms default=0"
    echo " * nmu-error-threshold defalut=128"
    echo " * nmu-train-threshold defalut=90"
    echo " * nmu-garbage-collection-ms default=0"
    echo " * nmu-max-collision default=40"
    echo " * nmu-minimal-coverage default=45"
    echo " * nmu-instant-remainder default=true"
    echo " * log-interval-ms default=1000"
fi

# Set the configuration of $1 to be $2
# @param $1 NuevoMatch configuration parameter
# @param $2 Value
function set_value() {
    file="$scripts_dir/.ovs-config"
    # Create a new file
    if [[ ! -e $file ]]; then
        echo "ovs-vsctl --no-wait" >> $file
        chmod +x $file
    fi
    # Set a new configuration
    if [[ $(grep "$1=" $file | wc -l) -eq 0 ]]; then
        echo "-- set Open_vSwitch . other_config:$1=$2" >> $file
    # Edit an existing configuration
    else
        sed -Ei "s/($1)=[^\\]+/\1=$2/g" "$file"
    fi
    # Set line breaks
    data=$(cat $file | sed 's/\\//g' | sed -E 's/\s*$/ \\/g' | sed '$s/\\//')
    echo "$data" > $file
}

if [[ $(get_flag default) -eq 1 ]]; then
    set_value "nmu-cool-down-time-ms" 0
    set_value "nmu-error-threshold" "128"
    set_value "nmu-train-threshold" "90"
    set_value "nmu-garbage-collection-ms" "0"
    set_value "nmu-max-collision" "40"
    set_value "nmu-minimal-coverage" "45"
    set_value "nmu-instant-remainder" "true"
    set_value "log-interval-ms" "1000"
    exit 0
fi


for (( i=1; i<=$#; i+=2 )); do
    if (( i+1 > $# )); then
        echo "Cannot set argument ${@:$i:1} - no matching value"
        exit 1
    fi
    name=${@:i:1}
    val=${@:$i+1:1}
    set_value $name $val
done
