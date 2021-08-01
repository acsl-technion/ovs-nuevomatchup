# Directories
export base_dir=$(readlink -f $(dirname $0)/..)
export scripts_dir=$base_dir/scripts

# Read configuration variables
if [[ ! -e $scripts_dir/.config ]]; then
    echo "Configuration file was not set." \
         "Make sure to build the project."
    exit 1
fi

# Read configuration
source $scripts_dir/.config

# Data dir and subdirs
export data_dir=$base_dir/data
export ruleset_dir=$data_dir/rulesets
export locality_dir=$data_dir/locality
export size_dir=$data_dir/size
export timestamp_dir=$data_dir/timestamp
export generated_dir=$data_dir/generated
export log_dir=$data_dir/logs

mkdir -p $generated_dir 2>/dev/null
mkdir -p $locality_dir 2>/dev/null
mkdir -p $log_dir 2>/dev/null
mkdir -p $size_dir 2>/dev/null
mkdir -p $timestamp_dir 2>/dev/null

# Store background PIDs in file, for being able to kill
# them from any other process that uses this script
bg_pids=$scripts_dir/.bgpid

# Access the script's command line arguments
commandline_args="$@"

# Append $2 to $1
function append() {
    echo -e "$1\n$2"
}

# Converts all whitespace to a single space
function uncolumn() {
    sed -E 's/\s+/ /g' <&0
}

# Appends the row index to the begging of the line
function append_row_index() {
    awk '{print NR, $0}' <&0
}

# If input is not empty, bypass it. Otherwise, output $1. Works per line.
# @input Data
# @param $1 Default value
function default() {
    cat - | awk -v def=$1 'NF{print $0; x=1}NF==0{print def; x=1}\
                           END{if (x==0) print def;}'
}

# Run tool in foreground, append to log, check output status.
# Timeout of 4 hours
# @param $1 log filename
# @param $2 commnad to run
# @param $3... parameters
function run_fg() {
    header="\n=====================\n" 
    header+="Command: $2 ${@:3}\n"
    header+="Date: $(date -u +"%Y-%m-%d %H:%M:%S")\n" 
    header+="Timeout: 4 hours\n"
    header+="=====================\n"
    echo -e "$header" >> $1

    eval timeout --foreground 4h $2 "${@:3}" >> $1 2>&1

    # Set end status and print message
    status=${PIPESTATUS[0]}
    if (( $status == 124 )); then status_val="timeout";
    elif (( $status == 0 )); then status_val="success";
    else status_val="error"; fi

    footer="Job finished with status '$status_val' on: "
    footer+="$(date -u +"%Y-%m-%d %H:%M:%S")\n"
    echo -e "$footer" >> $1
}

# Run tool in background, append to log, check output status.
# Timeout of 4 hours
# @param $1 log filename
# @param $2 commnad to run
# @param $3... parameters
function run_bg() {
    run_fg "$@" &
    pid=$!
    echo $pid >> $bg_pids
}

# Run a function in the background. No Timeout.
# @param $@ function name & params
function call_bg() {
    # Make the function not part of this subprocess group
    set -m
    eval $@ &
    echo $! >> $bg_pids

    # Return normal behavior
    set +m
}

# Wait for all subprocesses to complete
function wait_for_all() {
    [[ ! -e $bg_pids ]] && return
    pids=$(cat $bg_pids)
    rm $bg_pids
    for p in $pids; do
        wait $p &>/dev/null
    done
}

# Kill all processes in the background
# @param $1 Interrupt type (default SIGTERM)
function kill_all() {
    t=SIGTERM
    [[ ! -z $1 ]] && t=$1
    [[ ! -e $bg_pids ]] && return

    pids=$(cat $bg_pids)
    rm $bg_pids
    for p in $pids; do
        echo "Killing $p..."
        kill -s $t $p &>/dev/null
        wait $p &>/dev/null
    done
}

# Echos 1 is PID $1 lives
function pid_lives() {
    x=$(ls /proc/ | grep $pid)
    [[ -z $x ]] && echo 0 && return
    echo 1
}

# Get a list of valid background PIDs
function get_pids() {
    pids=$(cat $bg_pids 2>/dev/null)
    out=
    for pid in $pids; do      
        if [[ $(pid_lives $pid) -eq 1 ]]; then
            out=$(echo -e "$pid\n$out")
        else
            sed -i "/$pid/d" $bg_pids
        fi
    done
    echo "$out"
}

# Wait until number of subprocesses is smaller than threshold
# @param $1 threshold
function wait_for_threshold() {
  
    pids=$(get_pids) 
    counter=$(echo "$pids" | wc -l) 

    # Wait for processes
    while [[ $counter -ge $1 ]]; do
        for pid in $pids; do
            [[ $(pid_lives $pid) -ne 1 ]] && break
            sleep 1s
        done
        pids=$(get_pids) 
        counter=$(echo "$pids" | wc -l) 
    done
}

# Check if one of the scripts arguments contains "flag".
# Returns the value of the next argument after "flag", or
# 1 of the next argument is another flag
# If several arguments equal to "falg", returns the value
# of the last one.
# @param $1 Flag to check
# @return if found: 1 or value of next argument, otherwise empty string
function get_flag() {
    cmd_arg="(\s*[\'\\\"]((\\\\\\\"|[^\\\"])+)['\\\"]|\s*([^ \\\"\']+))"
    exists=$(echo "$commandline_args" | grep "\-\-$1" | wc -l)
    pattern="s/^.*--${1}${cmd_arg}.*/\2\4/g"
    next=$(echo "$commandline_args" | sed -E "$pattern")
    [[ $(echo "$next" | sed -E 's/^\s*//g' | cut -c1-2) == "--" ]] && next=""
    [[ $exists -eq 1 && -z $next ]] && echo 1
    [[ $exists -eq 1 && ! -z $next ]] && echo $next
}

# Calculate the mean of the input
function mean() {
    awk '{x+=$1}END{if (NR==0) print 0; else print x/NR;}' <&0
}

# Calculate the geomean of the input
function geomean() {
    awk 'BEGIN{x=1}{x*=$1}END{if (NR>0) print x^(1/NR); else print 0}' <&0
}

# Filter out values larger than $1
# @input Data, one column per row
# @param $1 Max acceptable value
function filter_out() {
    cat - | awk -v m=$1 '$1<m'
}

# Calculate the standatd deviation of the input
function stddev() {
 awk 'BEGIN{x=0; i=0; sd=0;} \
 {x+=$1; s[i]=$1; i++} \
 END{ \
  mean=x/NR; \
  for (i=0;i<NR;++i) { \
   sd+=(s[i]-mean)^2 \
  }; \
  print (sd/NR)^0.5;\
  }' <&0
}

# Multiply each input row by a factor
# @param $1 multiply factor
function multiply() {
    awk -v f=$1 '{print $1*f}' <&0
}

# Calculate the gaps between rows
function calc_gaps() {
    awk 'NR==1{c=$1} NR>1{gap+=($1-c);c=$1} END{if (NR==1) print 0; else \
         print gap/(NR-1)}' <&0
}

# Perform geomean per column over all rows
function geomean_per_column() {
    awk 'NR==1 {                              \
             for (i=1;i<=NF;++i) x[i]=1;      \
         }                                    \
         {                                    \
             for (i=1;i<=NF;++i) x[i]*=$i;    \
         }                                    \
         END { for (i=1;i<=NF;++i) {          \
                  printf "%.3f ", x[i]^(1/NR);\
               }                              \
               print "";                      \
         }' <&0
}

# Perform geomean per column over all indexed-rows
# with the same first column value (index)
# @returns "index" "geomean..."
function geomean_per_column_indexed() {
    sort -n <&0 | \
    awk '{if(seen[$1]==0) {                                \
              if (NR > 1) {                                \
                  printf "%d ", $1-1;                      \
                  for (i=2;i<=NF;++i) {                    \
                      printf "%.3f ", x[i]^(1/seen[$1-1]); \
                  }                                        \
                  print "";                                \
              }                                            \
              for (i=2;i<=NF;++i) x[i]=1;                  \
          }                                                \
          for (i=2;i<=NF;++i) x[i]*=$i;                    \
          seen[$1]++;                                      \
         }                                                 \
         END {                                             \
              printf "%d ", $1;                            \
              for (i=2;i<=NF;++i) {                        \
                  printf "%.3f ", x[i]^(1/seen[$1]);       \
              }                                            \
              print "";                                    \
         }'
}

# Perform mean per column over all rows
function mean_per_column() {
    awk 'BEGIN {                              \
             for (i=1;i<=NF;++i) x[i]=0;      \
         }                                    \
         {                                    \
             for (i=1;i<=NF;++i) x[i]+=$i;    \
         }                                    \
         END { for (i=1;i<=NF;++i) {          \
                  printf "%.3f ", x[i]/NR;    \
               }                              \
               print "";                      \
         }' <&0
}

# Compute the stddev per column over all rows
function stddev_per_column() {
    data=$(cat -)
    mean=$(echo "$data" | mean_per_column)
    data=$(append "$mean" "$data")
    echo "$data" | awk \
        'NR==1 {                                 \
             for (i=1;i<=NF;++i) {               \
                m[i]=$i;                         \
                x[i]=0;                          \
             }                                   \
         }                                       \
         {                                       \
             for (i=1;i<=NF;++i)                 \
                 x[i]+=($i-m[i])^2;              \
         }                                       \
         END { for (i=1;i<=NF;++i) {             \
                  printf "%.3f ", sqrt(x[i]/NR); \
               }                                 \
               print "";                         \
         }'
}

# Format mean(+-stddev) per column
function statistics_per_column() {
    data=$(cat -)
    mean=$(echo "$data" | mean_per_column)
    stddev=$(echo "$data" | stddev_per_column)
    echo "$mean" "$stddev" | awk ' {                \
        for (i=0; i<NF/2; ++i ) {                   \
            mean=i+1; stddev=i+1+NF/2;              \
            printf "%.3f(+-%.3f) ", $mean, $stddev; \
        }                                           \
        print "";                                   \
    }'
}

# Compute the sum of the first column
function sum() {
    awk '{x+=$1}END{print x}' <&0
}

# Compute sum per column over all rows
function sum_per_column() {
    awk '{                                    \
            for (i=1;i<=NF;++i) x[i]+=$i;     \
         }                                    \
         END {                                \
            for (i=1;i<=NF;++i) {             \
                printf "%.3f ", x[i];         \
            }                                 \
            print "";                         \
         }' <&0
}

# Compute the sum of data by its index
# Input line format: "index data"
# Output format: "index sum"
function sum_per_column_indexed() {
    sort -n <&0 | \
    awk '{if(seen[$1]==0) {                                \
              if (NR > 1) {                                \
                  printf "%d ", $1-1;                      \
                  for (i=2;i<=NF;++i) {                    \
                      printf "%.3f ", x[i];                \
                  }                                        \
                  print "";                                \
              }                                            \
              for (i=2;i<=NF;++i) x[i]=0;                  \
          }                                                \
          for (i=2;i<=NF;++i) x[i]+=$i;                    \
          seen[$1]++;                                      \
         }                                                 \
         END {                                             \
              printf "%d ", $1;                            \
              for (i=2;i<=NF;++i) {                        \
                  printf "%.3f ", x[i];                    \
              }                                            \
              print "";                                    \
         }'
}

# Compute the CDF values of the requested column
# @input data rows
# @param $1 The requested column for computing the CDF
function compute_cdf() {
    data=$(cat - | awk -v c=$1 '{print $c}' | sort -n)
    lines=$(echo "$data" | wc -l )
    echo "$data" | awk -v l=$lines '{print $1, NR/l}'
}

# Smooths the data points by a moving average with size $1.
# @input data points
# @param $1 window size
# @output The smoothed points
function smooth_by_moving_average() {
    awk -v w=$1 '{i=(NR-1)%w; x[i] = $1 }                            \
                 NR>w { t=0; for (i=0; i<w; ++i) t+=x[i]; print t/w} \
                 NR<w {print $1}' <&0
}

# Smooths the data points by a moving average with size $1, per column
# @input data points. All rows should have the same number of columns
# @param $1 window size
# @output The smoothed points
function smooth_by_moving_average_per_column() {
    awk -v w=$1 '                      \
        {                              \
            i=(NR-1) % w;              \
            for (f=1; f<=NF; f++) {    \
                x[i,f] = $f;           \
            }                          \
        }                              \
        NR<w {                         \
            for (f=1; f<=NF; f++) {    \
                t=0;                   \
                for (i=0; i<w; ++i) {  \
                    t+=x[i,f];         \
                }                      \
                printf "%.3f ", t/NR;  \
            }                          \
            printf "\n";               \
        }                              \
        NR>w {                         \
            for (f=1; f<=NF; f++) {    \
                t=0;                   \
                for (i=0; i<w; ++i) {  \
                    t+=x[i,f];         \
                }                      \
                printf "%.3f ", t/w;   \
            }                          \
            printf "\n";               \
        }                              \
    ' <&0
}

# Extract the values from the first column
# @input data
# @output Unique values of the first column
function extract_first_col_values() {
    sudo awk '{print $1}' <&0 2>/dev/null | sort | uniq
}

# Push values (${@:2:}) to queue ($1)
# Usage example: queue_push queue a b c d
function queue_push() {
    cmd="$1=\"\$$1 ${@:2}\""
    eval "$cmd"
}

# Pops from queue ($1) X values ($2) into variable $3
# Usage example: queue_pop queue 2 dst
function queue_pop() {
    values=$(eval "echo \"\$$1\"")
    values=$(echo "$values" | sed -E 's/^\s+//g') 
    (( from=$2+1 ))
    vals=$(echo "$values" | cut -d" " -f1-$2)
    new_queue=$(echo "$values" | cut -d" " -f$from-)
    eval "$1=\"$new_queue\""
    eval "$3=\"$vals\""
}

# Wait for any command on port
# @param $1 port 
# @param $2 msg
function tcp_wait() {
    echo -n "(waiting for $2 on port $1)..."
    rec=
    while [[ $rec != $2 ]]; do
        rec=$(sudo nc -l $1)
    done
    echo "okay"
}

# Get the next command from port
# @param $1 port
function tcp_get() {
    sudo nc -l $1
}

# Send command to remote server
# @param $1 ip
# @param $2 port 
# @param $3 msg
function tcp_send() {
    echo -n "(sending $3 on port $2 to ip $1)..."
    value=
    while [[ -z $value ]]; do
        value=$(echo $3 2>&1 > /dev/tcp/$1/$2)
    done
    echo "okay"
}


# Ask question, wait for answer
# @param $1 message
# @param ... list of valid answers
function ask() {
    valid="${@:2}"
    arr=($valid)
    valid=$(echo "$valid" | sed 's/\s/\//g')
    n=${#arr[@]}

    while :
    do
        read -p "$1 [$valid]: " ans
        okay=0
        for (( i=0; i<n; ++i )); do
            [[ $ans == ${arr[i]} ]] && okay=1 && break
        done
        [[ $okay -eq 0 ]] && echo "Invalid answer" >&2
        [[ $okay -eq 1 ]] && break
    done 
    echo $ans
}

# Check for errors in log file
# @param $1 filename
function check_errors() {
    # Get all relevant data
    data=$(cat "$1" | sed -n '/Configuration:/,$p' | sed '/Final OVS/q')
    # Remove common warning and errors
    data=$(echo "$data"| grep -v "datapath flow limit reached") # DP Limit
    data=$(echo "$data"| grep -v "ms waiting for")    # RCU Block
    data=$(echo "$data"| grep -v "Unreasonably long") # Long pool
    data=$(echo "$data"| grep -Pv "context switches:|faults:") # Long pool
    data=$(echo "$data"| grep -v "Invalid state transition for ukey") # nmflow
    data=$(echo "$data"| grep -v "Dropped") # Dropped log messages

    # Srach for errors
    [[ $(echo "$data" | grep "ERR" | wc -l) -ne 0 ]] &&
        echo "Errors in log $1" >&2 && return 1
    [[ $(echo "$data" | grep "WARN" | wc -l) -ne 0 ]] &&
        echo "Warnings in log $1" >&2 && return 1
}

