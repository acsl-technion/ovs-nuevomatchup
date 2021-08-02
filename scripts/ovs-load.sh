#!/bin/bash

# Must run this as root
[[ $EUID -ne 0 ]] && \
    echo "Please run me as root." && \
    exit 1

# Load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh

# Default arguments
smc_enabled=false
emc_enabled=false
ccache_enabled=false
cflows_enabled=false
nmu_enabled=false
source $scripts_dir/ovs-common.sh

# Require the ruleset name
if [[ ! -z $(get_flag help) ]]; then
    echo "Loads a given ruleset to a running instance of OVS"
    echo "Usage: $0 --ruleset RULESET [options]" 
    echo "--emc: Start OVS with EMC"
    echo "--smc: Start OVS with SMC"
    echo "--ovs-ccache: Start OVS with computational cache"
    echo "--ovs-cflows: Start OVS with computational flows"
    echo "--n-revalidator: Number of revalidator threads"
    echo "--n-handler: Number of handler threads"
    echo "--n-rxq: Number of DPDK RX queues"
    ovs_load_rules_help
    exit 1
fi

# Parse arguments
[[ -z $ruleset ]] && echo "Argument --ruleset is missing" && exit 1
[[ ! -z $(get_flag emc) ]] && emc_enabled=true
[[ ! -z $(get_flag smc) ]] && smc_enabled=true
[[ ! -z $(get_flag ovs-ccache) ]] && ccache_enabled=true && nmu_enabled=true
[[ ! -z $(get_flag ovs-cflows) ]] && cflows_enabled=true && nmu_enabled=true

n_revalidator=$(get_flag n-revalidator)
n_handler=$(get_flag n-handler)
n_rxq=$(get_flag n-rxq)

[[ -z $n_rxq ]] && n_rxq=1
[[ -z $n_revalidator ]] && n_revalidator=0
[[ -z $n_handler ]] && n_handler=0

# Check ruleset is valid
if [[ ! -d $generated_dir/$ruleset ]]; then
    echo "Cannot find ruleset \"$ruleset\" in \"$generated_dir\""
    exit 1
fi

# Check that ovs flows file exists
ovs_flows=$generated_dir/$ruleset/ovs.txt
if [[ ! -e $ovs_flows ]]; then
	echo "Ruleset has not been processed: cannot find $ovs_flows."
	exit 1
fi

# Delete exising bridges
echo "Deleting any existing bridges..."
$ovs_vsctl del-br $ovs_br

# Set PMD thread mask..
pmd_mask=$(echo $n_rxq | awk '{printf "0x%x\n", 2^$1-1}')

# Apply NuevoMatch configuration
if [[ -e $scripts_dir/.ovs-config ]]; then
    echo "Import NuevoMatchUp configuration from file..."
    # Print configuration
    cat $scripts_dir/.ovs-config              | \
    grep ".other_config"                      | \
    sed -E 's/^.*other_config:/ * /g;s/\\//g' | \
    tee -a $ovs_log_file
    # Apply configuration
    $scripts_dir/.ovs-config
fi

# Set number of revalidator/handler threads
echo "Using $n_revalidator revalidator and $n_handler handler threads"
ovs-vsctl --no-wait \
    set Open_vSwitch . other_config:n-revalidator-threads=$n_revalidator -- \
    set Open_vSwitch . other_config:n-handler-threads=$n_handler

# Apply configuration
ovs-vsctl --no-wait \
   set Open_vSwitch . other_config:emc-enable=$emc_enabled -- \
   set Open_vSwitch . other_config:smc-enable=$smc_enabled -- \
   set Open_vSwitch . other_config:nmu-enable=$nmu_enabled

if [[ $cflows_enabled == true ]]; then
    echo "Enabling Computational Flows, disabling revalidators"
    ovs-vsctl --no-wait \
       set Open_vSwitch . other_config:nmu-use-cmpflows=true  -- \
       set Open_vSwitch . other_config:max-revalidator=500000 -- \
       set Open_vSwitch . other_config:nmu-cmpflows-bridge-name=$ovs_br
else
    ovs-vsctl --no-wait \
       set Open_vSwitch . other_config:nmu-use-cmpflows=false
fi

# Print messages to log
echo "*** Configuration: " | tee -a $ovs_log_file
echo "*** emc_enabled=$emc_enabled" | tee -a $ovs_log_file
echo "*** smc_enabled=$smc_enabled" | tee -a $ovs_log_file
echo "*** ccache_enabled=$ccache_enabled" | tee -a $ovs_log_file
echo "*** cflows_enabled=$cflows_enabled" | tee -a $ovs_log_file

# Add ovs bridge and bind to interface (defined in .config)
$ovs_vsctl add-br $ovs_br                   \
 -- set bridge $ovs_br datapath_type=netdev

# Configure PMD threads
echo "Configuring DPDK with PMD mask $pmd_mask and $n_rxq RX queues..."
$ovs_vsctl \
    set Open_vSwitch . other_config:pmd-cpu-mask="$pmd_mask"

# Add run-to-completion ports
$ovs_vsctl \
 -- add-port $ovs_br "port-rx"              \
 -- set Interface "port-rx" type=dpdk       \
    options:dpdk-devargs=$pci_rx            \
    options:n_rxq=$n_rxq
$ovs_vsctl add-port $ovs_br "port-tx"       \
 -- set Interface "port-tx" type=dpdk       \
    options:dpdk-devargs=$pci_tx

# Set maximum number of DP flows to be 200K (default)
sudo $ovs_appctl upcall/set-flow-limit 200000

ovs_load_rules
