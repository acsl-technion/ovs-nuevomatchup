#!/bin/bash

# Must run this as root
[[ $EUID -ne 0 ]] && \
    echo "Please run me as root." && \
    exit 1

# Load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh
source $scripts_dir/ovs-common.sh

# Default arguments
autorun=false
wait_enabled=false

# Require the ruleset name
if [[ ! -z $(get_flag help) ]]; then
    echo "Starts DPDK-OVS with custom configuration"
    echo "Usage: $0 --ruleset RULESET --cores CORES [options]"
    echo "--autorun: Do not open less, run OVS and exit script"
    echo "--wait: Do not load rules before the user press ENTER"
    echo "--cores: How many cores to assign to OVS (e.g., 4)"
    echo "--llc: How many ways to assign to LLC using Intel CAT (e.g., 0x000f)"
    ovs_load_rules_help
    exit 1
fi

# Parse arguments
[[ ! -z $(get_flag autorun) ]] && autorun=true
[[ ! -z $(get_flag wait) ]] && wait_enabled=true

cores=$(get_flag cores)
llc=$(get_flag llc)
[[ -z $cores ]] && echo "--cores argument is missing!"

echo "*** ovs-start.sh script start with arguments $@"

# Stop OVS, delete bridge
echo "Stopping any running instance of OVS..."
$scripts_dir/ovs-stop.sh

# Bind DPDK drivers
$scripts_dir/dpdk-init.sh

# Starts ovsdb-server server (w/o vswitchd)
echo "Starting ovsdb server..."
rm -f $ovs_log_file
$ovs_ctl --no-ovs-vswitchd  \
         --system-id=random \
         --delete-bridges   \
         start              \
&> /dev/null

# Configure DPDK 
echo "Configuring OVS with DPDK, 2 PMD threads..."
$ovs_vsctl --no-wait \
    set Open_vSwitch . other_config:dpdk-init=true -- \
    set Open_vSwitch . other_config:pmd-cpu-mask="0x0003"

# Start vswitchd
echo "Starting OVS vswitchd..."
$ovs_ctl --no-ovsdb-server             \
         --db-sock="$ovs_db_sock_file" \
         start                         \
&> /dev/null

# Validate OVS has loaded DPDK
result=$(sudo ovs-vsctl get Open_vSwitch . dpdk_initialized)
if [[ $result != true ]]; then
	echo "Error: dpdk is not enabled in OVS. Exiting OVS."
    $scripts_dir/ovs-stop.sh
    exit 1
fi

# Set CPU affinity
echo "Settings OVS affinity to cores 1-$cores"
taskset -cp "1-$cores" $(pgrep "ovs-vswi")

# Set LLC using Intel CAT
if [[ ! -z $llc ]]; then
    echo "Set LLC to $llc using Intel CAT..."
    numcpu=$(cat /proc/cpuinfo | grep processor | wc -l)
    (( lastcpu=$numcpu-1 ))
    pqos -e "llc:1=$llc"
    pqos -a "llc:1=0-$lastcpu;"
fi

[[ $wait_enabled == true ]] && read -p "[Press ENTER to continue]"

# Manual mode, show log, wait for user interrupt
if [[ $autorun == false ]]; then

    sudo less -SN $ovs_log_file

    # Stop bg processes w/ SIGTERM
    kill_all SIGTERM

    # Stop OVS, delete bridge
    $scripts_dir/ovs-stop.sh
fi

