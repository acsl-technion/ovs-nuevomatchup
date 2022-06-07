# Introduction                                                                     
The official source code of the paper "Scaling Open vSwitch with a Computational Cache" (USENIX NSDI, 2022).

This repo contains:                      
   * An OVS module that works with [libnuevomatchup](https://alonrashelbach.com/libnuevomatchup). The module's source code is available in [dpif-netdev-nmu.c](dpif-netdev-nmu.c) and [dpif-netdev-nmu.h](dpif-netdev-nmu.h).
   * Patches to OVS for supporting the module ([ovs.patch](ovs.patch)).
   * Various scripts for reproducing the paper's results.                          
                                                                                   
# Reproducing the results                                                          
                                                                                   
Connect two machines back-to-back using DPDK supported NICs. The machines should also be connected to a shared LAN (e.g., via another NIC). This is essential for running the scripts.
The *System Under Test* (SUT) machine must have an Intel CPU that supports both the AVX and POPCNT extensions.
You must have a Linux OS with root permissions in both machines for building the environment.
Contact me for a link to the ruleset artifacts, or create one of your own using  [these tools](https://alonrashelbach.com/2021/12/20/benchmarking-packet-classification-algorithms/).
### SUT machine
Run ```sudo ./build-sut.sh```. This script downloads the relevant DPDK and OVS versions, installs *libnuevomatchup*, patches and compiles OVS, and installs it.

### *Load Generator* (LGEN) machine
Run ```sudo ./build-lgen.sh```. This scripts downloads our [custom made packet generator](https://github.com/alonrs/simple-packet-gen).

### Running the experiments
Run ```sudo ./scripts/run-experiments.sh sut``` on the SUT machine. The script will wait for instructions from the LGEN machine.

Run ```sudo ./scripts/run-experiments.sh lgen NAME``` on the LGEN machine, where NAME is the experiment name to run, such as `thr-caida-3m`.

# License

The OVS module is licensed under the MIT license.
