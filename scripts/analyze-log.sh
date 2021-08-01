#!/bin/bash

# Load common script
source $(readlink -f $(dirname $(readlink -f $0)))/common.sh

if [[ $(get_flag help) -eq 1 ]]; then
    echo "Usage: $0 ruleset experiment [options]"
    echo "--last: get the drop percent of the last experiment in the" \
         "temporary log file"
    echo "--debug: show raw log after inital processing"
    echo "--result: show a complete list of results"
    echo "--thold: custom threshold for boostrapping. Default is 5 Mp"
fi

# In case the first argument is a file
if [[ -e $1 ]]; then
    ruleset="$(basename $(dirname $1))"
    experiment="$(basename $1)"
# Read ruleset and experiment name
else
    ruleset=$1
    experiment=$2
fi

logfile="$generated_dir/$ruleset/$experiment"
last=$(get_flag last)
thold=$(get_flag thold)
show_result=$(get_flag result)
show_debug=$(get_flag debug)

[[ -z $thold ]] && thold=0

if [[ $last -eq 1 ]]; then
    logfile=$scripts_dir/.lgenout.tmp
    if [[ ! -e $logfile ]]; then
        echo "Temporary log file does not exist"
        exit 1
    fi
fi

if [[ ! -e $logfile ]]; then
    echo "Logfile \"$logfile\" does not exist"
    exit 1
fi

# Extrace data from logfile
data=$(grep -Pi "reloading|tx rate|^tx|^rx" $logfile |\
       sed 's/^.*Reloading/Reloading/g'  |\
       grep -v "nan %" | grep -v "inf %" |\
       sed 's/TX rate of/TX rate/g'      |\
       sed "s/' '/'baseline'/g")

if [[ ! -z "$show_debug" ]]; then
    echo "$data"
    exit 0
fi

# Calculate average; ignore first 5 Mp (except in ultra-low TX rates)
result=$(echo "$data" | \
         awk '\
         func print_stats() {
              if (!adaptive) {
                thold=total*0.05;
              } else {
                thold=0;
              }
              rx=0;
              cnt=0;
              drop=0;
              start=0;
              for (i=0; i<counter; i++) {
                old_start=start
                start+=rx_arr[i];
                if (old_start < thold) {
                    continue;
                }
                rx+=rx_arr[i];
                drop+=drop_arr[i];
                cnt++;
              }
              if (cnt != 0) {
                avg_drop=drop/cnt;
              } else {
                avg_drop=100;
                cnt=1;
              }
              printf "%s: %s avg RX: %.3f avg drop: %.3f "\
                     "RX/TX %.0f %% Thold %.3f Mp\n",
                     method, msg,rx/cnt, avg_drop,
                     rx/cnt/tx*100, thold;
         }
         /Reloading OVS with/ { 
              if (method != $7 && NR > 2) {
                print_stats();
              }
              method=$7;
          } 
          /Reset with/ { 
              if (NR>2) {
                print_stats();
              }
              adaptive = ($3 == "adaptive");
              msg=$0;
              tx=$6/1000;
              total=0;
              counter=0;
              delete rx_arr; delete drop_arr;
          } 
          /^RX/ {
              rx_arr[counter]=$2;
              drop_arr[counter]=$11;
              counter++;
          }
          /^TX/ {
              total+=$2;
          }
          END {
              print_stats();
          }')


methods=$(echo "$result" | cut -d" " -f1 | sort | uniq)
options=$(echo "$data" | grep options | cut -d" " -f 9- | head -1)

# Echo all results
if [[ ! -z "$show_result" ]]; then
    for method in $methods; do
        echo "$result" | grep $method | sort -nrk 7
    done
    exit 0
fi

# Echo the drop percent of the last experiment
if [[ $last -eq 1 ]]; then
    echo "$result" | grep "constant" | tail -1 | cut -d" " -f14
    exit 0
fi

# Echo options
echo "$options"

# Get all methods
for method in $methods; do
    # Beautify
    method=$(echo "$method" | sed "s/'//g;s/://g;s/--//g")
    # Echo the highest constant RX rate with drop below 1%
    cnst=$(echo "$result" | grep "$method" | sort -nrk 7 | \
           grep constant | awk '$14<=1' | sort -nk 11 | tail -1)
    # Validate that there is an RX rate with drop > 1%
    valid=$(echo "$result" | grep "$method" | sort -nrk 7 | \
            grep constant | awk '$14>1{x=1}END{if (x!=1) print ", *glitch*"}')
    
    # Echo the adaptive rate
    adap=$(echo "$result" | grep "$method" | \
           grep adaptive | sort -nrk 10 | head -1)
    vals=($(echo "$cnst" "$adap" | cut -d" " -f7,11,14,30))
    vals[0]=$(echo ${vals[0]} | awk '{print $1/1000}')
    
    echo "Ruleset $ruleset experiment $experiment"        \
         "method $method max const rate: ${vals[1]} Mpps" \
         "(drop: ${vals[2]} %,TX: ${vals[0]} Mpps$valid);"\
         "Adaptive: ${vals[3]} Mpps"
done
