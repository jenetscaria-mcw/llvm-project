#!/bin/bash
# theoretical_cycles_v2.sh - Fixed to compile full module, then extract functions
# Usage: ./theoretical_cycles_v2.sh <module.ll>

set -e

INPUT=$1

# if [ -z "$INPUT" ]; then
#     echo "Usage: $0 <module.ll>"
#     exit 1
# fi

# # Check for required tools
# if ! command -v llc &> /dev/null; then
#     echo "Error: llc not found. Install LLVM toolchain."
#     exit 1
# fi

# if ! command -v llvm-mca &> /dev/null; then
#     echo "Error: llvm-mca not found. Install LLVM toolchain."
#     exit 1
# fi

# echo "======================================"
# echo "Theoretical Cycle Analysis"
# echo "======================================"
# echo "Input: $INPUT"
# echo ""

# # Step 1: Compile full module to assembly
# echo "[1/4] Compiling full module to assembly..."
# mkdir -p asm_analysis

# BASENAME=$(basename "$INPUT" .ll)
# ASM_FILE="asm_analysis/${BASENAME}_full.s"
ASM_FILE=$1

# Need to fix flags correctly for the target architecture - check -mcpu=native correct or not
# llc -mcpu=native "$INPUT" -o "$ASM_FILE"
# echo "   Generated: $ASM_FILE"

# Step 2: Find versioned functions in assembly
echo ""
echo "[2/4] Finding versioned functions..."
# Match _v followed by one or more digits
FUNCS=$(grep -E "^.*_v[0-9]+:" "$ASM_FILE" | sed 's/://g' | sort -u)

if [ -z "$FUNCS" ]; then
    echo "   No versioned functions found in assembly"
    exit 1
fi

# Get function list from assembly file
echo "Found:"
echo "$FUNCS" | head -5
FUNC_COUNT=$(echo "$FUNCS" | wc -l)
if [ $FUNC_COUNT -gt 5 ]; then
    echo "... and $((FUNC_COUNT - 5)) more"
fi

# Step 3: Extract each version from assembly and dump in separate files
echo ""
echo "[3/4] Extracting individual versions..."

declare -A EXTRACTED_FUNCS
declare -A BASE_VERSIONS  # Track versions per base function

for FUNC in $FUNCS; do
    # Get base name and version - handle multi-digit versions
    # Extract version number (one or more digits after _v)
    VER=$(echo "$FUNC" | grep -oE '_v[0-9]+$' | sed 's/_v//')
    BASE=$(echo "$FUNC" | sed "s/_v${VER}$//")
    
    if [ -n "$VER" ] && [ -n "$BASE" ]; then
        # Extract this function's assembly
        # From function label to next .size directive
        OUTPUT="asm_analysis/${BASE}_v${VER}.s"
        
        sed -n "/^${FUNC}:/,/^\.size.*${FUNC}/p" "$ASM_FILE" > "$OUTPUT"
        
        if [ -s "$OUTPUT" ]; then
            echo "   ${BASE}_v${VER}"
            EXTRACTED_FUNCS["$BASE,$VER"]="$OUTPUT"
            # Track which versions exist for each base function
            BASE_VERSIONS["$BASE"]="${BASE_VERSIONS[$BASE]} $VER"
        fi
    fi
done

# Step 4: Run llvm-mca on each version
echo ""
echo "[4/4] Running llvm-mca..."
echo ""

mkdir -p mca_reports

# Organize by base function
declare -A BASE_FUNCS
for KEY in "${!EXTRACTED_FUNCS[@]}"; do
    BASE=$(echo "$KEY" | cut -d',' -f1)
    BASE_FUNCS["$BASE"]=1
done

echo "======================================"
echo "Cycle Estimates"
echo "======================================"
echo ""
echo "Found $(echo "${!BASE_FUNCS[@]}" | wc -w) base function(s):"
for BASE in "${!BASE_FUNCS[@]}"; do
    VCOUNT=$(echo "${BASE_VERSIONS[$BASE]}" | wc -w)
    echo "  - $BASE ($VCOUNT version(s))"
done
echo ""

for BASE in "${!BASE_FUNCS[@]}"; do
    echo "Function: $BASE"
    echo "─────────────────────────────────────────────────────────────────────"
    printf "%-12s %12s %12s %12s\n" "Version" "Cycles" "IPC" "Instructions"
    echo "─────────────────────────────────────────────────────────────────────"
    
    BEST_CYCLES=999999
    BEST_VER=""
    
    # Get all versions for this base function and sort them numerically
    VERSIONS=$(echo "${BASE_VERSIONS[$BASE]}" | tr ' ' '\n' | sort -n)
    
    for v in $VERSIONS; do
        KEY="$BASE,$v"
        ASM_FILE="${EXTRACTED_FUNCS[$KEY]}"
        
        if [ -n "$ASM_FILE" ] && [ -f "$ASM_FILE" ]; then
            # Run llvm-mca
            # Sanitize base name for filename (replace / with _)
            SAFE_BASE=$(echo "$BASE" | sed 's/\//_/g')
            MCA_OUTPUT="mca_reports/${SAFE_BASE}_v${v}.txt"
            llvm-mca -mcpu=native "$ASM_FILE" > "$MCA_OUTPUT" 2>&1 || true
            
            # Extract metrics
            CYCLES=$(grep "Total Cycles:" "$MCA_OUTPUT" | awk '{print $3}' || echo "N/A")
            IPC=$(grep "^IPC:" "$MCA_OUTPUT" | awk '{print $2}' || echo "N/A")
            INSTRS=$(grep "Total Instructions:" "$MCA_OUTPUT" | awk '{print $3}' || echo "N/A")
            
            # Track best
            if [ "$CYCLES" != "N/A" ] && [ "$CYCLES" -lt "$BEST_CYCLES" ]; then
                BEST_CYCLES=$CYCLES
                BEST_VER="v$v"
            fi
            
            # Highlight best
            if [ "$CYCLES" = "$BEST_CYCLES" ] && [ "$CYCLES" != "N/A" ]; then
                printf "\033[32m%-12s %12s %12s %12s\033[0m\n" "v$v" "$CYCLES" "$IPC" "$INSTRS"
            else
                printf "%-12s %12s %12s %12s\n" "v$v" "$CYCLES" "$IPC" "$INSTRS"
            fi
        fi
    done
    
    echo "─────────────────────────────────────────────────────────────────────"
    if [ -n "$BEST_VER" ]; then
        echo "Best (theoretical): $BEST_VER with $BEST_CYCLES cycles"
    fi
    echo ""
done

echo "======================================"
echo "Analysis Complete"
echo "======================================"
echo ""
echo "Generated files:"
echo "  asm_analysis/       - Assembly files"
echo "  mca_reports/        - Detailed llvm-mca reports"
echo ""
echo "For details:"
echo "  less mca_reports/*.txt"
