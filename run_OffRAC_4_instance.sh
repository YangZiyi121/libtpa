#!/bin/bash

# Base command parameters
BASE_CMD="TPA_ETH_DEV=enp194s0f1np1 TPA_CFG=\"tcp {tso = 0; }\" tpa run build/bin/app/tperf -c 172.24.5.108 -p 2888 -d 10 -n 1 -t rr -Z 1"
LOG_DIR="vision_data/OffRAC_4_instance"

# Array of parameters for each process
M_VALUES=(1024 1024 1024 1024)
F_VALUES=(1 6 2 4)           # Varying -F
R_VALUES=(64 64 64 64)        # Varying -R
X_VALUES=(4096 4096 4096 4096)    # Varying -X

# Check that arrays have the same length
if [ ${#F_VALUES[@]} -ne ${#R_VALUES[@]} ] || [ ${#F_VALUES[@]} -ne ${#X_VALUES[@]} ]; then
    echo "Error: Parameter arrays must have the same length"
    exit 1
fi

# Launch four processes in parallel
for i in {0..3}; do
    F=${F_VALUES[$i]}
    R=${R_VALUES[$i]}
    X=${X_VALUES[$i]}
    M=${M_VALUES[$i]}

    TPA_ID="client_$((i))"
    # Construct the log file name
    START_CPU=$((i * 7))
    
    LOG_FILE="$LOG_DIR/m_${M}_X_${X}_n_4_f_${F}_O_1.log"
    
    # Full command
    CMD="sudo TPA_ID=$TPA_ID $BASE_CMD -m $M -F $F -R $R -X $X -S $START_CPU | tee $LOG_FILE"
    
    echo "Launching process $((i+1)): $CMD"
    eval "$CMD" &
    
    # Store the process ID
    PIDS[$i]=$!
done

# Wait for all background processes to complete
echo "All processes launched. Waiting for completion..."
wait ${PIDS[@]}

echo "All processes completed."
