# Introduction
The official source code of the paper "Scaling Open vSwitch with a
Computational Cache" (USENIX NSDI, 2022). This repo contains:
   * An OVS module that works with [libnuevomatchup](https://alonrashelbach.com/libnuevomatchup)
   * Various scripts for reproducing the paper's results.

# Reproducing the results

Connect two machines back-to-back using DPDK supported NICs.
The *System Under Test*(SUT) machine must have an Intel CPU that
supports the AVX and POPCNT extensions.


