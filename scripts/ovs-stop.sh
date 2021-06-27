#!/bin/bash

# Must run this as root
[[ $EUID -ne 0 ]] && \
    echo "Please run me as root." && \
    exit 1

# Load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh
source $scripts_dir/ovs-common.sh

# Set OVS aliases
ovs_ctl=$(which ovs-ctl)
ovs_vsctl=$(which ovs-vsctl)
ovsdb_tool=$(which ovsdb-tool)
ovsdb_server=$(which ovsdb-server)
ovs_vswitchd=$(which ovs-vswitchd)
ovs_ofctl=$(which ovs-ofctl)

# Reset any previous traps on SIGINT..
trap "exit 1" SIGINT

echo "Killing processes created by ovs-start.sh"
kill_all SIGTERM

echo "*** Stopping OVS *** " | sudo tee -a $ovs_log_file

$ovs_ctl --db-sock="$ovs_db_sock_file" --delete-bridges stop
$ovs_ctl --db-sock="$ovs_db_sock_file" --delete-bridges status

# Make sure ovs is killed
kill $(pgrep ovs-vs) 2>/dev/null
kill -9 $(pgrep ovs-vs) 2>/dev/null

$scripts_dir/mem-clean.sh

# Copy last OVS log 
cp $ovs_log_file $log_dir/ovs.log
chmod +r $log_dir/ovs.log

