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

# Analysis function
analyze_log=$scripts_dir/analyze-log.sh

# Stop on SIGINT
function stopall() {
    trap "echo Received SIGINT, terminating process..." "INT"
    pgrep client.exe | xargs kill 2>/dev/null
    sleep 1
    pgrep client.exe | xargs kill -9 2>/dev/null
    kill $tail_pid 2>/dev/null
    rm -f $lgen_pipe 2>/dev/null
    rm -f $lgen_output 2>/dev/null
    # Make sure LGEN is killed
    pgrep lgen-start | xargs -i kill -9 {} 2>/dev/null
    pgrep client.txt | xargs -i kill -9 {} 2>/dev/null
    trap "exit 1" "INT"
    send-sut ./scripts/ovs-stop.sh
}
trap stopall "INT"

expr_list="cores throughput"
rulesets=$(ls $ruleset_dir | grep -P "acl|fw|ipc" | sort -n)

# Show usage message
if [[ $# -lt 1 ]]; then
    echo "Usage: $0 sut | lgen [expr]"
    echo "'expr' (experiment list) may be one or more of the following:" \
         $expr_list
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
[[ ! -z $2 ]] && expr_list=$2
echo "Got experiment list: '$expr_list'"

# Returns 0 iff the experiment in arg1 is available
function check-expr() {
    if echo $expr_list | grep $@ > /dev/null; then
        echo "Performing experiment $@"
        experiment="$@"
        return 0
    else
        return 1
    fi
}

# Returns 0 iff the ruleset $1 has no experiment ${@:2:}
function check-ruleset() {
    if [[ ! -d $generated_dir/$1/ ]]; then
        echo "Ruleset $1 is invalid"
        return 1
    elif [[ -e $generated_dir/$1/${@:2} ]]; then
        echo "Experiment ${@:2} exists for ruleset $1"
        return 1
    else
        echo "Performing on ruleset $1"
        return 0
    fi
}

# Commit ruleset: move the experiment temporary log file to its permanent
# location.
# @param $1 ruleset
# @param $2... experiment name
function commit-ruleset() {
    echo "Comitting ruleset $1, experiment ${@:2}..."
    if [[ ! -e $lgen_output ]]; then
        echo "Error - LGEN output does not exist!"
        return 1
    fi
    mv $lgen_output $generated_dir/$1/${@:2}
}

# Set the next LGEN signal according to the drop percent of the last
# experiment
function lgen-set-options() {
    # Reset TX rates
    echo "Lgen set options with status $lgen_options_status"
    if [[ $lgen_options_status -eq 0 ]]; then
        lgen_options_status=1
        lgen_options_hi=14000
        lgen_options_lo=0
        lgen_options_mid=0
        lgen_options=" (mid = $mid)"
        drop_okay=0
    # Calculate drop percent
    elif [[ $lgen_options_status -eq 1 ]]; then
        drop=$($analyze_log --last 2>/dev/null)
        if [[ $? -eq 0 ]]; then
            drop_okay=$(echo $drop | awk '{if($1>=-100 && $1<1) print 1;
                                           else print -1;}')
            echo "Drop percent of last experiment: $drop (status: $drop_okay)"
        else
            drop_okay=0
            echo "Cannot determine drop from last experiment"
        fi
    # Stop after adaptive experiment
    elif [[ $lgen_options_status -eq 2 ]]; then
        lgen_options_status=0
        lgen_options=""
        return
    # Custom experiment ?
    elif [[ $lgen_options_status -eq 3 ]]; then
        queue_pop lgen_options_custom 2 lgen_options
        return
    fi
    # Drop is okay, try higher speed
    if [[ $drop_okay -eq 1 ]]; then
        lgen_options_lo=$lgen_options_mid
    # Drop is not okay, try lower speed
    elif [[ $drop_okay -eq -1 ]]; then
        lgen_options_hi=$lgen_options_mid
    fi
    # Calculate next const TX rate
    mid=$(echo "$lgen_options_hi $lgen_options_lo" | awk \
          '{print int(($1+$2)/2/50)*50}')
    # Finish in case mid has not changed
    if [[ $mid -eq 0 || $mid -eq $lgen_options_mid ]]; then
        # Try adaptive TX
        echo "Set adaptive TX rate (mid = $mid)"
        lgen_options="a 1000"
        lgen_options_status=2
        lgen_options_mid=0
    # Try another experiment with const TX
    else
        echo "Set constant TX rate with TX $mid Kpps"
        lgen_options="t $mid"
        lgen_options_mid=$mid
    fi
}

# Wait for lgen script
function lgen-wait() {
    while [[ $(pgrep lgen-start | wc -l) -gt 0 ]]; do
        wait $lgen_pid
    done
    kill $tail_pid 2>/dev/null
}

# Catches signal SIGUSR1 sent by lgen script.
function lgen-signal() {
    sleep 0.3
    lgen-set-options

    # Save last log
    if [[ ! -z $last_pipe_val ]]; then
        methodlog=$(echo "$last_method" | sed 's/--//g;s/ //g')
        load_optionslog=$(echo "$last_load_options" | sed 's/--//g;s/ /-/g')
        start_optionslog=$(echo "$last_start_options" | sed 's/--//g;s/ /-/g')
        extra_options_log=$(echo "$last_extra_options")
        send-sut "./scripts/ovs-copy-log.sh $ruleset $experiment $methodlog  \
                                            $last_pipe_val $start_optionslog \
                                            $load_optionslog \
                                            $extra_options_log"
    fi

    # No more options, exit LGEN
    if [[ -z $lgen_options ]]; then
        echo "LGEN options empty; stopping LGEN"
        echo "s" > $lgen_pipe
        return
    fi

    # Get next speed
    queue_pop lgen_options 2 pipe_val

    # Store previous parameters for OVS log file
    last_pipe_val=$pipe_val
    last_method=$method
    last_load_options="$load_options"
    last_start_options="$start_options"
    last_extra_options="$extra_options"

    # Load rules in SUT
    send-sut "./scripts/ovs-load.sh $emc_flag --ruleset $ruleset $method" \
             "$load_options"

    # Do another manual command on SUT - depends on the experiment
    if [[ ! -z $lgen_manual_command ]]; then
        send-sut "$lgen_manual_command"
    fi

    # Echo experiment in log
    echo "e Reloading OVS with ruleset $ruleset" \
         "method '$method' options '$load_options'" > $lgen_pipe
    # Get next value, send to LGEN
    echo "Sending '$pipe_val' to LGEN"
    echo $pipe_val > $lgen_pipe
}

# Load generator
lgen_pipe=$scripts_dir/.lgen.tmp
lgen_output=$scripts_dir/.lgenout.tmp
lgen_options_status=0
lgen_manual_command=""
lgen_pid=0
tail_pid=0
method=
load_options=
start_options=
extra_options=

# Start packet generator. Both 'lgen_pipe' and 'lgen_pid' are used to 
# communicate with the packet generator. The communication is done via the
# 'lgen-signal' method that is invoked using signals from the lgen app.
function lgen() {
    echo "Loading packet generator..."
    rm -f $lgen_pipe   2>/dev/null
    mkfifo $lgen_pipe
    trap lgen-signal "USR1"
    if [[ -z $lgen_options_custom ]]; then
        lgen_options_status=0
    else
        lgen_options_status=3
    fi
    last_pipe_val=""
    $scripts_dir/lgen-start.sh $@ --signal $$ < $lgen_pipe >> $lgen_output &
    lgen_pid=$!
    tail -f $lgen_output &
    tail_pid=$!
    echo "*" > $lgen_pipe
    lgen-wait
    echo "Packet generator has stopped"
    rm -f $lgen_pipe 2>/dev/null
}

# Make sure LGEN is killed
pgrep lgen-start | xargs -i kill -9 {} &>/dev/null

# Configure default arguments
send-sut ./scripts/ovs-config.sh --default
emc_flag="--emc"

# Perform a manual experiment
if check-expr manual; then
    read -p "Locality:" locality
    read -p "IPG:" ipg
    read -p "Rulesets:" rulesets
    read -p "Start options:" start_options
    read -p "Load options:" load_options
    read -p "Methods:" methods
    read -p "Custom speed configuration:" lgen_options_custom
    read -p "Custom OVS configuration:" custom_ovs_config
    read -p "Custom experiment name:" experiment

    # Change experiment name for log
    [[ -z $experiment ]] && experiment="manual-$(date +"%Y%m%d-%H%M%S")"
    
    if [[ ! -z $custom_ovs_config ]]; then
        send-sut ./scripts/ovs-config.sh $custom_ovs_config
    fi

    for ruleset in $rulesets; do
       send-sut ./scripts/ovs-start.sh --autorun $start_options
       for method in "$methods"; do
           lgen --ruleset $ruleset --locality $locality --ipg $ipg
       done
       commit-ruleset $ruleset $experiment
    done
fi

# Perform LLC experiment
if check-expr llc; then
    # Set constants for these experiments
    locality=caida-3m
    ipg=caida-3m-diff
    rulesets="fw4-100k fw4-500k"
    load_options="--n-handler 1 --n-revalidator 1 --n-rxq 1"
    for ruleset in $rulesets; do
        for llc in 0x1 0x7 0x1f 0x7f 0x1ff 0x7ff; do
            if ! check-ruleset $ruleset llc-$llc; then
                continue
            fi
            start_options="--cores 2 --llc $llc"
            # Check all methods
            for method in --ovs-ccache --ovs-cflows; do
                # Initiate load generator
                send-sut ./scripts/ovs-start.sh --autorun $start_options
                lgen --ruleset $ruleset --locality $locality --ipg $ipg
            done
            commit-ruleset $ruleset llc-$llc
        done
    done
fi


# Perform cores experiment
if check-expr cores; then
    # Set constants for these experiments
    locality=caida-3m
    ipg=caida-3m-diff
    rulesets="ipc1-1k"
    for ruleset in $rulesets; do
        for cores in 2 3 4 5 6 7 8 9 10; do
            if ! check-ruleset $ruleset cores-$cores; then
                continue
            fi
            # Start OVS
            start_options="--cores $cores"
            send-sut ./scripts/ovs-start.sh --autorun $start_options
            (( n_rxq=$cores-1 ))
            load_options="--n-handler 1 --n-revalidator 1 --n-rxq $n_rxq"
            # Check all methods
            for method in --ovs-orig --ovs-ccache --ovs-cflows; do
                # Initiate load generator
                lgen --ruleset $ruleset --locality $locality --ipg $ipg
            done
            commit-ruleset $ruleset cores-$cores
        done
    done
fi

# Perform zipf experiment
if check-expr zipf; then
    # Set constants for these experiments
    rulesets="acl1-1k acl2-1k acl5-1k"
    start_options="--cores 2"
    load_options="--n-handler 1 --n-revalidator 1 --n-rxq 1"
    for ruleset in $rulesets; do
        for alpha in 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9; do
            if ! check-ruleset $ruleset thr-zipf-$alpha; then
                continue
            fi
            locality=zipf-$alpha
            ipg=zipf-$alpha-diff
            send-sut ./scripts/ovs-start.sh --autorun $start_options
            for method in --ovs-orig --ovs-ccache; do
                lgen --ruleset $ruleset --locality $locality --ipg $ipg
            done
            commit-ruleset $ruleset thr-zipf-$alpha
        done
    done
fi

# Perform updates experiment
if check-expr update; then
    # Set constants for these experiments
    locality=caida-3m
    ipg=caida-3m-diff
    rulesets="fw4-500k"
    initial=500
    start_options="--cores 2"
    method="--ovs-cflows"
    emc_flag=""
    send-sut ./scripts/ovs-config.sh log-interval-ms 50
    for ruleset in $rulesets; do
        for instant in true false; do
            send-sut ./scripts/ovs-config.sh instant-remainder $instant
            for rate in 100; do
                extra_options=instant-$instant-initial-$initial-rate-$rate
                name=update-$extra_options
                if ! check-ruleset $ruleset $name; then
                    continue
                fi
                # Start OVS
                send-sut ./scripts/ovs-start.sh --autorun $start_options
                
                # Add manual command to trigger alongside ovs-load.sh on SUT
                lgen_manual_command="./scripts/ovs-rate.sh "
                lgen_manual_command+="--ruleset $ruleset --rate $rate --interval 10000 "
                lgen_manual_command+="--initial-delay 10 --initial-rules $initial "
                lgen_manual_command+="--signal \$\$ --do-not-delete"

                load_options="--initial-rules $initial"
                lgen_options_custom="t 2500"
                # Initiate load generator
                lgen --ruleset $ruleset --locality $locality --ipg $ipg
                commit-ruleset $ruleset $name
            done
        done
    done
fi

# Perform throughput experiment
# @param $1 trace name
function throughput-expr() {
    if check-expr thr-$1; then
        # Set constants for these experiments
        locality=$1
        ipg=${1}-diff
        start_options="--cores 2"
        load_options="--n-handler 1 --n-revalidator 1 --n-rxq 1"
        send-sut ./scripts/ovs-start.sh --autorun $start_options
        # For all rulesets...
        for ruleset in $rulesets; do
            if ! check-ruleset $ruleset thr-$1; then
                continue
            fi
            # Check all methods
            for method in --ovs-orig --ovs-ccache --ovs-cflows; do
                # Initiate load generator
                lgen --ruleset $ruleset --locality $locality --ipg $ipg
            done
            commit-ruleset $ruleset thr-$1
        done
    fi
}

throughput-expr caida-3m
throughput-expr mawi-15
throughput-expr zipf-0.9
throughput-expr zipf-0.6

# Stop all experiments
stopall
