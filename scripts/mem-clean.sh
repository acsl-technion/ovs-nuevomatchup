#!/bin/bash

# Must run this as root
[[ $EUID -ne 0 ]] && \
    echo "Please run me as root." && \
    exit 1

rm -f /dev/hugepages/rtemap*
