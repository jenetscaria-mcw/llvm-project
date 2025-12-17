#!/bin/bash

# Script to run adaptive sampling 5 times and analyze results
# Save this as run_adaptive_analysis.sh

# Configuration
MODEL_PATH="/mnt/Data/Jenet/ggml-model-q4_0.gguf"
# PROMPT="Once upon a time"
# SEED=2345
EXECUTABLE="./llama-bench"
THREADS=1
# EXECUTABLE="./llama-cli -m /mnt/Data/Jenet/ggml-model-q4_0.gguf -p $STATEMENT -s $SEED"
NUM_RUNS=5
OUTPUT_DIR="adaptive_runs_4_versions"
RESULTS_FILE="adaptive_results_4V.txt"

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Initialize results associative array (requires bash 4.0+)
declare -A best_versions

# Function to extract function names and best versions from log
analyze_log() {
    local log_file="$1"
    local run_number="$2"
    
    echo "=== Analyzing Run $run_number ===" >> "$RESULTS_FILE"
    
    # Extract function names and best versions using grep and sed
    while IFS= read -r line; do
        if [[ $line =~ \[PATCH\]\ ([^[:space:]]+)\ -\ Function\ pointer\ patched\ to\ V([0-9]+) ]]; then
            function_name="${BASH_REMATCH[1]}"
            best_version="${BASH_REMATCH[2]}"
            
            echo "Function: $function_name -> Best Version: V$best_version" >> "$RESULTS_FILE"
            
            # Store in associative array
            if [[ -z "${best_versions[$function_name]}" ]]; then
                best_versions[$function_name]="V$best_version"
            else
                best_versions[$function_name]="${best_versions[$function_name]}, V$best_version"
            fi
        fi
    done < "$log_file"
    
    echo "" >> "$RESULTS_FILE"
}

# Clear previous results
> "$RESULTS_FILE"

echo "Running adaptive executable $NUM_RUNS times..."
echo "Results will be saved in: $OUTPUT_DIR/"
echo "Summary will be saved in: $RESULTS_FILE"
echo ""

# Run the executable multiple times
for ((i=1; i<=NUM_RUNS; i++)); do
    log_file="$OUTPUT_DIR/run_${i}.log"
    
    echo "Run $i/$NUM_RUNS - Logging to: $log_file"
    
    # Run the executable and capture output
    # $EXECUTABLE -m "$MODEL_PATH" -p "$PROMPT" -s $SEED -t $THREADS> "$log_file" 2>&1
    $EXECUTABLE -m "$MODEL_PATH" -t $THREADS> "$log_file" 2>&1
    # $EXECUTABLE -m "$MODEL_PATH" > "$log_file" 2>&1

    # Analyze the log file
    analyze_log "$log_file" "$i"
    
    echo "Run $i completed"
    echo "---"
done

# Generate final summary
echo "=== FINAL SUMMARY ===" >> "$RESULTS_FILE"
echo "Function Name : {Best Versions Chosen Across $NUM_RUNS Runs}" >> "$RESULTS_FILE"
echo "=========================================" >> "$RESULTS_FILE"

# Sort function names for consistent output
while IFS= read -r function; do
    if [[ -n "$function" ]]; then
        printf "%-40s : {%s}\n" "$function" "${best_versions[$function]}" >> "$RESULTS_FILE"
    fi
done < <(printf "%s\n" "${!best_versions[@]}" | sort)

echo ""
echo "Analysis complete!"
echo "Individual run logs: $OUTPUT_DIR/run_[1-5].log"
echo "Summary results: $RESULTS_FILE"

# Display final summary on screen
echo ""
echo "=== FINAL SUMMARY ==="
echo "Function Name : {Best Versions Chosen Across $NUM_RUNS Runs}"
echo "========================================="
while IFS= read -r function; do
    if [[ -n "$function" ]]; then
        printf "%-40s : {%s}\n" "$function" "${best_versions[$function]}"
    fi
done < <(printf "%s\n" "${!best_versions[@]}" | sort)