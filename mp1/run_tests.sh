#!/bin/bash

set -u

SENDER=./sender_20221599
NETSIM=./netsim

MAX_BYTES=100000000

run_test() {

    local file=$1
    local ber=$2
    local seed=$3

    local base
    base=$(basename "$file")

    local outfile="out_${base}_ber${ber}_seed${seed}"
    local logfile="log_${base}_ber${ber}_seed${seed}.txt"

    echo ""
    echo "=================================================="
    echo "FILE : $file"
    echo "BER  : $ber"
    echo "SEED : $seed"
    echo "=================================================="

    START=$(date +%s)

    $NETSIM $SENDER \
        --input "$file" \
        --output "$outfile" \
        --ber "$ber" \
        --seed "$seed" \
        --max_bytes "$MAX_BYTES" \
        > "$logfile" 2>&1

    END=$(date +%s)

    grep -E \
    "status:|bytes_total:|frames_total:|frames_acked:|frames_naked:|cost:" \
    "$logfile"

    if cmp -s "$file" "$outfile"; then
        echo "CMP RESULT : PASS"
    else
        echo "CMP RESULT : FAIL"
    fi

    echo "TIME : $((END - START)) sec"

    echo "=================================================="
}

echo ""
echo "###############################"
echo "# LOW BER BASELINE"
echo "###############################"

run_test harry_potter.txt 0 1
run_test harry_potter.txt 1e-6 1

echo ""
echo "###############################"
echo "# HIGH BER ANALYSIS"
echo "###############################"

run_test harry_potter.txt 1e-4 1
run_test harry_potter.txt 1e-4 2
run_test harry_potter.txt 1e-4 3

run_test harry_potter.txt 1e-3 1
run_test harry_potter.txt 1e-3 2
run_test harry_potter.txt 1e-3 3

echo ""
echo "###############################"
echo "# BINARY ROBUSTNESS"
echo "###############################"

run_test cat_bgm.mp3 1e-4 1
run_test cat_bgm.mp3 1e-3 1