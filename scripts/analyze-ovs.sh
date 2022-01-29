#!/bin/bash

# load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh

rulesets="acl1 acl2 acl3 acl4 acl5 fw1 fw2 fw3 fw4 fw5 ipc1 ipc2"

if [[ ! -z $(get_flag help) ]]; then
    echo "Analyzes OVS run logs"
    echo "Usage: $0 --experiment EXPR --size SIZE --method METHOD"
    echo "--experiment: Name of experiment to analyze"
    echo "--size: Size category of rulesets (e.g., 100k)"
    echo "--method: The method to test; e.g., ovs-orig"
    exit 1
fi

size=$(get_flag size)
experiment=$(get_flag experiment)
method=$(get_flag method)

if [[ -z $size || -z $experiment || -z $method ]]; then
    echo "One of the flags is missing"
    exit 1
fi

speeds=
for r in $rulesets; do
    read -p "Enter speed for ruleset $r in Kpps:" ans
    speeds+="$ans "
done

rulesets=($rulesets)
speeds=($speeds)
n=${#speeds[@]}

for (( i=0; i<$n; i++ )); do
    log=$(find "$generated_dir"/${rulesets[$i]}-$size \
               -name "$experiment*-t-${speeds[$i]}*" |\
          grep $method | head -1)
    [[ -z $log ]] && continue
    data=($(grep Extended $log | tail -1))
    
    echo "${rulesets[$i]}-$size: ${data[11]} subtables"
done
