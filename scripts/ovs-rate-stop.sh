#!/bin/bash

echo "Stopping rate script..."
pgrep ovs-rate.sh | xargs kill 2>/dev/null

