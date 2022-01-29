#!/bin/bash

# load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh

if [[ ! -z $(get_flag help) ]]; then
    echo "Analyzes OVS run logs for update charts"
    echo "Usage: $0 --ruleset RULESET --experiment NAME [options]"
    echo "--ruleset: Name of ruleset to analyze"
    echo "--experiment: Name of experiment to analyze"
    echo "--align VALUE: Align the first insert time to value. default: 0"
    echo "--smooth VALUE: Smooth data points using moving aberage"
    echo "--raw: Show raw data"
    echo "--show: Show semi-processed info"
    exit 1
fi

ruleset=$(get_flag ruleset)
expr=$(get_flag experiment)
align=$(get_flag align | default 0)
smooth=$(get_flag smooth | default 1)

# Log interval ms
interval=50

if [[ -e $1 ]]; then
    log="$1"
else
    if [[ -z $ruleset || -z $expr ]]; then
        echo "Error: one of the arguments is missing."
        exit 1
    fi
    log="$generated_dir/$ruleset/$expr"
fi

if [[ $(get_flag raw) -eq 1 ]]; then
    cat $log | grep -P "Extended|new classifier"
    exit 0
fi

# Extract raw data from OVS log
# @param $1 Log filename
# @param $2 Move seconds by
function process_raw_data() {
    local m=$(echo $2 | default 0)
    # time, insertions, throughput, coverage
    cat $1 | grep -P "Extended|new classifier" | awk \
    -v m=$m -v interval=$interval '
        /new/{print $0}
        /Extended/{printf "Stats %7.2f %7d %7.3f %7.2f\n",
                   x*(interval/1000)+m, $4, $10/(interval/1000)/1e6, $34;x++}'
}

# Extract throughput data
# @input Raw data
function extract_throughput() {
    local data=$(cat - | awk '/Stats/{print $2, $4}')
    local vals=$(echo "$data" | smooth_by_moving_average $smooth 2)
    local time=$(echo "$data " | awk '{print $1}')
    paste -d" " <(echo "$time") <(echo "$vals") |\
    awk '$1>=0' |\
    column -t
}

# Extract coverage data
# @input Raw data
function extract_coverage() {
    cat - | awk '/Stats/{print $0}' | \
    awk '$2>=0{print $2, $5}' | \
    column -t
}

function extract_insert_time() {
    cat - | awk '/Stats/ && $2>9 && $3>0 {print $2; exit 0}'
}

data=$(process_raw_data $log)
insert_time=$(echo "$data" | extract_insert_time)

if [[ $align -ne 0 ]]; then
    move=$(echo $align-$insert_time | bc)
else
    move=0
fi

data=$(process_raw_data $log $move)
if [[ $(get_flag show) -eq 1 ]]; then
    echo "$data"
    exit 0
fi

base=$(basename $log)

echo "Saving throughput to ${base}-tpt.txt"
echo "$data" | extract_throughput > ${base}-tpt.txt

echo "Saving coverage to ${base}-cov.txt"
echo "$data" | extract_coverage > ${base}-cov.txt
