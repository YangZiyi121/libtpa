#!/bin/bash

# Base command parameters
#sudo TPA_ID=client TPA_ETH_DEV=enp194s0f1np1 TPA_CFG="tcp {tso = 0; }" tpa run build/bin/app/tperf -c 172.24.5.50 -d 10 -n 4 -m 1024 -R 1024 -t rr -X 1024 -Z 0 -F 1 -S 0 -p 3000
BASE_CMD="TPA_ETH_DEV=enp194s0f1np1 TPA_CFG=\"tcp {tso = 0; }\" tpa run build/bin/app/tperf -c 172.24.5.50 -p 3000 -d 30 -t rr -Z 0"

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
    START_CPU=4
    
    LOG_FILE="rr_d_30_m_${M}_n_${N}_f_${F}_O_2"
    
    # Create directory and set permissions
    mkdir rr_d_30_m_${M}_n_${N}_f_${F}_O_2
    chmod -R 777 rr_d_30_m_${M}_n_${N}_f_${F}_O_2
    
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
