#!/bin/bash

# Base command parameters

#BASE_CMD="TPA_ETH_DEV=enp195s0f1np1 TPA_CFG=\"tcp {tso = 0; }\" tpa run build/bin/app/tperf -c 172.24.5.106 -p 2888 -d 30 -t rr -Z 1"
BASE_CMD="TPA_ETH_DEV=enp195s0f1np1 TPA_CFG=\"tcp {tso = 0; } net { listen_scaling = 0; }\" tpa run build/bin/app/tperf -c 172.24.5.108 -d 30 -t rr -Z 1 -p 2888 -A 172.24.5.50 -B 3000 -G"

# Array of parameters for each process
M_VALUES=(1024 1024 1024 1024)
F_VALUES=(12 1 1 1)           # Varying -F
R_VALUES=(1024 1024 1024 1024)        # Varying -R
X_VALUES=(1024 1024 1024 1024)    # Varying -X
N_VALUES=(20 1 1 1)

# Check that arrays have the same length
if [ ${#F_VALUES[@]} -ne ${#R_VALUES[@]} ] || [ ${#F_VALUES[@]} -ne ${#X_VALUES[@]} ] || [ ${#F_VALUES[@]} -ne ${#N_VALUES[@]} ]; then
    echo "Error: Parameter arrays must have the same length"
    exit 1
fi

# Launch four processes in parallel
for i in {0..0}; do
    F=${F_VALUES[$i]}
    R=${R_VALUES[$i]}
    X=${X_VALUES[$i]}
    M=${M_VALUES[$i]}
    N=${N_VALUES[$i]}

    TPA_ID="client_$((i))"
    # Construct the log file name
    START_CPU=$((i * 14))
    
    LOG_FILE="rr_d_30_m_${M}_n_${N}_f_${F}_O_1"
    
    # Create directory and set permissions
    mkdir rr_d_30_m_${M}_n_${N}_f_${F}_O_1
    chmod -R 777 rr_d_30_m_${M}_n_${N}_f_${F}_O_1
    
    # Full command
    CMD="sudo TPA_ID=$TPA_ID $BASE_CMD -n $N -m $M -F $F -R $R -X $X -S $START_CPU  -L 1 -D $LOG_FILE"
    
    echo "Launching process $((i+1)): $CMD"
    eval "$CMD" &
    
    # Store the process ID
    PIDS[$i]=$!
done

# Wait for all background processes to complete
echo "All processes launched. Waiting for completion..."
wait ${PIDS[@]}

echo "All processes completed."
