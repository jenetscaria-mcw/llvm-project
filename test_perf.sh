#!/bin/bash

MODEL="/mnt/Data/Jenet/ggml-model-q4_0.gguf"

echo "Waiting 5 seconds before starting first run..."
sleep 5

for i in {1..5}; do
    echo "Running iteration $i ..."
    
    ./llama-bench -m "$MODEL" -t 1 > "test_run_new_woV2_${i}.txt"
    
    if [ $i -lt 5 ]; then
        echo "Completed run $i, waiting 10 seconds..."
        sleep 10
    fi
done

echo "All runs completed!"