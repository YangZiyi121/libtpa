#!/bin/bash

# Example: Run server on 1 core but listen on 24 ports
# This demonstrates the new -P parameter

# USAGE:
# -n <threads>  : Number of CPU cores/threads to use
# -P <ports>    : Number of ports to listen on (NEW PARAMETER)
# -p <base_port>: Starting port for requests
# -A <address>  : Client address to send responses to
# -B <base_port>: Starting port for responses

# Example 1: 1 thread handling 24 ports
# Server listens on request ports 3000-3023 (24 ports)
# Server responds on ports 4000-4023 (24 ports)  
# All handled by a single thread on CPU core 0
sudo TPA_ID=server TPA_ETH_DEV=enp195s0f1np1 TPA_CFG="tcp {tso = 0; }" \
    tpa run build/bin/app/tperf \
    -s \
    -n 1 \
    -P 24 \
    -p 3000 \
    -A 172.24.5.16 \
    -B 4000 \
    -S 0

# Example 2: 4 threads handling 24 ports (load distributed across threads)
# All 4 threads can accept from all 24 request ports (3000-3023)
# All 4 threads can open response connections to all 24 response ports (4000-4023)
# Thread assignment is handled by the kernel/RSS
# sudo TPA_ID=server TPA_ETH_DEV=enp195s0f1np1 TPA_CFG="tcp {tso = 0; }" \
#     tpa run build/bin/app/tperf \
#     -s \
#     -n 4 \
#     -P 24 \
#     -p 3000 \
#     -A 172.24.5.16 \
#     -B 4000 \
#     -S 0

# Note: The 1:1 mapping is maintained between request and response ports:
# - Request received on port 3000 -> Response sent to port 4000
# - Request received on port 3001 -> Response sent to port 4001
# - Request received on port 3023 -> Response sent to port 4023
