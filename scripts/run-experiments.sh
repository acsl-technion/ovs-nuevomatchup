#!/bin/bash

# Must run this as root
[[ $EUID -ne 0 ]] && \
    echo "Please run me as root." && \
    exit 1

# Load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh

# Network functions
net=$scripts_dir/net.py
function send-sut() {
    $net send $sut_ip $@
}

expr_list="thr-static thr-dynamic"

# Show usage message
if [[ $# -lt 1 ]]; then
    echo "Usage: $0 sut | lgen [expr]"
    echo "'expr' (experiment list) may be one or more of the following: " \
         $expr_list
    echo "When 'expr' is empty, all experiments will be performed."
    exit 1
fi

# SUT machine waits for command from LGEN machine, then terminates
if [[ $1 == [Ss][Uu][Tt] ]]; then
    $net start-server
    exit 0
elif [[ $1 != [Ll][Gg][Ee][Nn] ]]; then
    echo "Argument $1 is nither LGEN nor SUT."
    exit 1
fi

# Test connection to SUT
if ! send-sut echo "LGEN has started" > /dev/null; then
    echo "Cannot connect to SUT machine; did you run 'run-experiments.sh'" \
         "on the machine with the IP '$sut_ip'?"
    exit 1
fi

# Which experiments will be performed
expr_list=$2
echo "Got experiment list $expr_list"

# Returns 0 iff the experiment in arg1 is available
function check-expr() {
    if echo $expr_list | grep $@ > /dev/null; then
        echo "Performing experiment $@"
        return 0
    else
        return 1
    fi
}

# Perform dynamic throughput experiment
if check-expr thr-dynamic; then
    send-sut ./scripts/ovs-start.sh --autorun
fi

