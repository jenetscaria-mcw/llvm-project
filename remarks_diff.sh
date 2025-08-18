#!/bin/bash

# LLVM RemarkUtil PGO vs NON-PGO Analysis Example
# This script demonstrates how to use llvm-remarkutil to analyze and compare
# optimization remarks between PGO and non-PGO builds

set -e

echo "=== LLVM RemarkUtil PGO vs NON-PGO Analysis ==="
echo "This script demonstrates comprehensive analysis using llvm-remarkutil"
echo ""

# Check if llvm-remarkutil is available
if ! command -v llvm-remarkutil &> /dev/null; then
    echo "Error: llvm-remarkutil not found. Please install LLVM tools."
    exit 1
fi

# Function to analyze remarks with llvm-remarkutil
analyze_remarks() {
    local file=$1
    local label=$2
    
    echo "=== $label Analysis ==="
    
    if [ ! -f "$file" ]; then
        echo "Warning: $file not found, skipping analysis"
        return
    fi
    
    echo "1. Total remark count:"
    llvm-remarkutil count "$file" --parser=yaml --group-by=total
    
    echo ""
    echo "2. Remarks by pass (top 10):"
    llvm-remarkutil count "$file" --parser=yaml --pass-name=".*" --group-by=pass --count-by=remark-name | \
        tail -n +2 | sort -t',' -k2 -nr | head -10
    
    echo ""
    echo "3. Remarks by type:"
    llvm-remarkutil count "$file" --parser=yaml --group-by=remark-type --count-by=remark-name
    
    echo ""
    echo "4. Function-level optimization activity:"
    llvm-remarkutil count "$file" --parser=yaml --group-by=function --count-by=remark-name | \
        tail -n +2 | sort -t',' -k2 -nr | head -5
    
    echo ""
    echo "5. Loop optimization analysis:"
    llvm-remarkutil count "$file" --parser=yaml --pass-name="loop-.*" --group-by=pass --count-by=remark-name
    
    echo ""
    echo "6. Inlining analysis:"
    llvm-remarkutil count "$file" --parser=yaml --pass-name="inline" --group-by=remark-type --count-by=remark-name
    
    echo ""
    echo "7. Vectorization analysis:"
    llvm-remarkutil count "$file" --parser=yaml --pass-name="vectorize" --group-by=remark-type --count-by=remark-name
    
    echo ""
    echo "8. Memory optimization analysis:"
    llvm-remarkutil count "$file" --parser=yaml --pass-name="licm|gvn|dse" --group-by=pass --count-by=remark-name
    
    echo "----------------------------------------"
}

# Function to compare PGO vs NON-PGO
compare_pgo_nonpgo() {
    local nonpgo_file=$1
    local pgo_file=$2
    
    echo "=== PGO vs NON-PGO Comparison ==="
    
    if [ ! -f "$nonpgo_file" ] || [ ! -f "$pgo_file" ]; then
        echo "Warning: One or both files not found, skipping comparison"
        return
    fi
    
    echo "1. Total remark count comparison:"
    echo "Non-PGO total:"
    llvm-remarkutil count "$nonpgo_file" --parser=yaml --group-by=total
    echo "PGO total:"
    llvm-remarkutil count "$pgo_file" --parser=yaml --group-by=total
    
    echo ""
    echo "2. Pass-by-pass comparison (top 10 most active):"
    echo "Pass Name | Non-PGO Count | PGO Count | Difference"
    echo "----------|---------------|-----------|------------"
    
    # Get all unique pass names
    local all_passes=$(llvm-remarkutil count "$nonpgo_file" --parser=yaml --pass-name=".*" --group-by=pass --count-by=remark-name | \
        tail -n +2 | cut -d',' -f1 | sort -u)
    
    for pass in $all_passes; do
        local nonpgo_count=$(llvm-remarkutil count "$nonpgo_file" --parser=yaml --pass-name="$pass" --group-by=total 2>/dev/null | \
            tail -n +2 | cut -d',' -f2 || echo "0")
        local pgo_count=$(llvm-remarkutil count "$pgo_file" --parser=yaml --pass-name="$pass" --group-by=total 2>/dev/null | \
            tail -n +2 | cut -d',' -f2 || echo "0")
        
        # Calculate difference
        local diff=$((pgo_count - nonpgo_count))
        printf "%-15s | %-13s | %-9s | %+d\n" "$pass" "$nonpgo_count" "$pgo_count" "$diff"
    done | sort -k4 -nr | head -10
    
    echo ""
    echo "3. Optimization success rate comparison:"
    echo "Pass Name | Non-PGO Success | PGO Success | Improvement"
    echo "----------|-----------------|-------------|-------------"
    
    for pass in "inline" "gvn" "licm" "loop-vectorize" "loop-unroll"; do
        local nonpgo_passed=$(llvm-remarkutil count "$nonpgo_file" --parser=yaml --pass-name="$pass" --remark-type=passed --group-by=total 2>/dev/null | \
            tail -n +2 | cut -d',' -f2 || echo "0")
        local nonpgo_total=$(llvm-remarkutil count "$nonpgo_file" --parser=yaml --pass-name="$pass" --group-by=total 2>/dev/null | \
            tail -n +2 | cut -d',' -f2 || echo "0")
        
        local pgo_passed=$(llvm-remarkutil count "$pgo_file" --parser=yaml --pass-name="$pass" --remark-type=passed --group-by=total 2>/dev/null | \
            tail -n +2 | cut -d',' -f2 || echo "0")
        local pgo_total=$(llvm-remarkutil count "$pgo_file" --parser=yaml --pass-name="$pass" --group-by=total 2>/dev/null | \
            tail -n +2 | cut -d',' -f2 || echo "0")
        
        if [ "$nonpgo_total" -gt 0 ] && [ "$pgo_total" -gt 0 ]; then
            local nonpgo_rate=$(echo "scale=1; $nonpgo_passed * 100 / $nonpgo_total" | bc -l 2>/dev/null || echo "0")
            local pgo_rate=$(echo "scale=1; $pgo_passed * 100 / $pgo_total" | bc -l 2>/dev/null || echo "0")
            local improvement=$(echo "scale=1; $pgo_rate - $nonpgo_rate" | bc -l 2>/dev/null || echo "0")
            printf "%-15s | %-15.1f%% | %-11.1f%% | %+.1f%%\n" "$pass" "$nonpgo_rate" "$pgo_rate" "$improvement"
        fi
    done
    
    echo "----------------------------------------"
}

# Function to generate detailed pass analysis
detailed_pass_analysis() {
    local file=$1
    local label=$2
    local pass_name=$3
    
    echo "=== $label - $pass_name Detailed Analysis ==="
    
    if [ ! -f "$file" ]; then
        echo "Warning: $file not found, skipping analysis"
        return
    fi
    
    echo "1. All remarks for $pass_name:"
    llvm-remarkutil count "$file" --parser=yaml --pass-name="$pass_name" --group-by=remark-type --count-by=remark-name
    
    echo ""
    echo "2. Function-level $pass_name activity:"
    llvm-remarkutil count "$file" --parser=yaml --pass-name="$pass_name" --group-by=function --count-by=remark-name | \
        tail -n +2 | sort -t',' -k2 -nr | head -10
    
    echo ""
    echo "3. $pass_name optimization details:"
    llvm-remarkutil count "$file" --parser=yaml --pass-name="$pass_name" --group-by=total --count-by=remark-name | \
        tail -n +2 | sort -t',' -k2 -nr
    
    echo "----------------------------------------"
}

# Main execution
main() {
    # Default file names - modify these as needed
    NONPGO_FILE="master_all.opt.yaml"
    PGO_FILE="PGO_all.opt.yaml"
    
    # Check if files exist, if not, try to find them
    if [ ! -f "$NONPGO_FILE" ]; then
        NONPGO_FILE=$(find . -name "*non_pgo*.opt.yaml" -o -name "*master*.opt.yaml" | head -1)
        if [ -z "$NONPGO_FILE" ]; then
            echo "Warning: No non-PGO remarks file found"
            NONPGO_FILE=""
        fi
    fi
    
    if [ ! -f "$PGO_FILE" ]; then
        PGO_FILE=$(find . -name "*pgo*.opt.yaml" | head -1)
        if [ -z "$PGO_FILE" ]; then
            echo "Warning: No PGO remarks file found"
            PGO_FILE=""
        fi
    fi
    
    echo "Using files:"
    echo "  Non-PGO: $NONPGO_FILE"
    echo "  PGO: $PGO_FILE"
    echo ""
    
    # Individual analysis
    if [ -n "$NONPGO_FILE" ]; then
        analyze_remarks "$NONPGO_FILE" "Non-PGO Build"
    fi
    
    if [ -n "$PGO_FILE" ]; then
        analyze_remarks "$PGO_FILE" "PGO Build"
    fi
    
    # Comparison analysis
    if [ -n "$NONPGO_FILE" ] && [ -n "$PGO_FILE" ]; then
        compare_pgo_nonpgo "$NONPGO_FILE" "$PGO_FILE"
        
        # Detailed analysis for key passes
        echo ""
        echo "=== Detailed Pass Analysis ==="
        for pass in "inline" "gvn" "licm" "loop-vectorize"; do
            detailed_pass_analysis "$NONPGO_FILE" "Non-PGO" "$pass"
            detailed_pass_analysis "$PGO_FILE" "PGO" "$pass"
        done
    fi
    
    echo ""
    echo "=== Analysis Complete ==="
    echo ""
    echo "Usage tips:"
    echo "1. Modify file names at the top of the script"
    echo "2. Use --help with llvm-remarkutil for more options"
    echo "3. Filter by specific passes using --pass-name"
    echo "4. Group by different criteria: --group-by=function,pass,remark-type"
    echo "5. Count by different metrics: --count-by=remark-name,function,pass"
}

# Run main function
main "$@"