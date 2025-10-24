#!/bin/bash
sudo TPA_ID=client TPA_ETH_DEV=enp194s0f1np1 TPA_CFG="tcp {tso = 0; }" tpa run build/bin/app/tperf -c 172.24.5.50 -d 10 -n 1 -m 1024 -R 1024 -t rr -X 1024 -Z 0 -F 12 -S 0 -p 3000
#start a server
#sudo TPA_ID=server TPA_ETH_DEV=enp195s0f1np1 TPA_CFG="tcp {tso = 0; }" tpa run build/bin/app/tperf -s -n 1 -S 1 -p 3000
