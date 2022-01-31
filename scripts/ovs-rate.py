#!/usr/bin/env python3

import sys
import os
import time
import re
import subprocess

timer_current=time.time_ns()
timer_counter=0
timer_total=0
timer_sleep=0

# Sample the current timestamp in nanoseconds.
# Returns number of seconds since last sample.
def timer_sample(interval):
    global timer_current, timer_counter, timer_total, timer_sleep
    timer_current_n=time.time_ns()
    timer_val=timer_curent_n-timer_current
    if timer_val < interval:
        timer_diff=interval-timer_val
    else
        timer_diff=0
    timer_counter=timer_counter+1
    timer_total=timer_total+timer_diff
    timer_sleep=timer_total/timer_counter
    timer_current=timer_current_n
    return round(timer_val/1e6)/1e3


def timer_sleep_ms():
    global timer_sleep
    time.sleep(timer_sleep/1e6)


def load_flows(ruleset, initial_rules):
    with f as open(ruleset, "r"):
        flows=reverse(f.readlines())
    total=len(flows)-initial_rules
    bse_flows=flows[0:initial_rules]
    add_flows=flows[initial_rules:]
    del_flows=re.sub('add','', add_flows)
    del_flows=re.sub(', prio.*','', del_flows)
    return total, add_flows, del_flows
            

def run(ruleset, rate, initial_rules, interval):
    total, add_flows, del_flows = load_flows(ruleset, initial_rules)
    print('*** Rate experiment - using {t} flows from {r}'.format(t=total,
                                                                  r=ruleset))
    f=add_flows[0:rate]
    for i in range(0, total, rate):
        timer_sample(interval)
        os.system('{ovs_ofctl} --bundle add-flow {ovs_br} -'.format(
                  os.environ['ovs_ofctl'],
                  os.environ['ovs_br'])
        timer_sleep_ms()
        os.system('{ovs_ofctl} --bundle del-flow {ovs_br} -'.format(
                  os.environ['ovs_ofctl'],
                  os.environ['ovs_br'])

