#!/bin/bash

# Universal profiler for llvm-lit output
# Handles: Olden, mafft, and other benchmarks with various argument styles

set -e

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Rerunning with sudo..."
    exec sudo -E "$0" "$@"
fi

# Configuration
LIT_OUTPUT="${1}"
OUTPUT_FILE="${2:-hotspots.txt}"
ANNOTATION_FILE="${3:-annotations.txt}"

if [ -z "$LIT_OUTPUT" ] || [ ! -f "$LIT_OUTPUT" ]; then
    echo "Error: llvm-lit output file not found"
    echo ""
    echo "Usage: $0 <lit_output_file> [hotspots_file] [annotations_file]"
    echo ""
    echo "Example:"
    echo "  llvm-lit -v -j 1 ... > output.txt 2>&1"
    echo "  $0 output.txt"
    exit 1
fi

echo "========================================="
echo "Universal Hotspot Profiler"
echo "========================================="
echo "Input: $LIT_OUTPUT"
echo "Output: $OUTPUT_FILE"
echo ""

# Step 1: Parse lit output
echo "[1/4] Parsing llvm-lit output..."

TEST_NAMES=()
EXECUTABLES=()
TEST_ARGS=()
WORK_DIRS=()
INPUT_FILES=()

while IFS= read -r line; do
    if echo "$line" | grep -q "timeit-target"; then
        # Extract working directory
        workdir=$(echo "$line" | sed 's/.*--chdir //' | awk '{print $1}')
        
        # Check for input redirection
        input_file=""
        if echo "$line" | grep -q -- "--redirect-input"; then
            input_file=$(echo "$line" | sed 's/.*--redirect-input //' | awk '{print $1}')
        fi
        
        # Extract executable and args (after .time)
        exe_and_args=$(echo "$line" | sed 's/.*\.time //')
        
        # Extract executable (first non-option field)
        exe=$(echo "$exe_and_args" | awk '{print $1}')
        
        # Extract test name
        testname=$(basename "$exe")
        
        # Extract arguments (everything after executable)
        args=$(echo "$exe_and_args" | sed "s|^$exe ||" | sed 's/^[ \t]*//')
        [ "$args" = "$exe" ] && args=""
        
        # Only add if executable exists
        if [ -x "$exe" ]; then
            TEST_NAMES+=("$testname")
            EXECUTABLES+=("$exe")
            TEST_ARGS+=("$args")
            WORK_DIRS+=("$workdir")
            INPUT_FILES+=("$input_file")
        fi
    fi
done < "$LIT_OUTPUT"

if [ ${#TEST_NAMES[@]} -eq 0 ]; then
    echo "   No tests found"
    echo ""
    echo "Make sure you ran: llvm-lit -v ... > output.txt 2>&1"
    exit 1
fi

echo "   Found ${#TEST_NAMES[@]} test(s):"
for i in "${!TEST_NAMES[@]}"; do
    printf "    %2d. %-20s" $((i+1)) "${TEST_NAMES[$i]}"
    [ -n "${TEST_ARGS[$i]}" ] && printf " (args: %s)" "${TEST_ARGS[$i]}"
    [ -n "${INPUT_FILES[$i]}" ] && printf " < %s" "$(basename "${INPUT_FILES[$i]}")"
    echo ""
done
echo ""

# Initialize output
cat > "$OUTPUT_FILE" << EOF
===========================================
Hotspot Analysis from llvm-lit Output
===========================================
Date: $(date)
Source: $LIT_OUTPUT
Tests: ${#TEST_NAMES[@]}

EOF

cat > "$ANNOTATION_FILE" << EOF
# Function Annotations from llvm-lit Profiling
# Generated: $(date)
# Source: $LIT_OUTPUT

EOF

# Step 2: Profile each test
echo "[2/4] Profiling..."
TEST_NUM=1
TOTAL=${#TEST_NAMES[@]}
ALL_HOTSPOTS=()
ALL_COUNTS=()

for i in "${!TEST_NAMES[@]}"; do
    testname="${TEST_NAMES[$i]}"
    exe="${EXECUTABLES[$i]}"
    args="${TEST_ARGS[$i]}"
    workdir="${WORK_DIRS[$i]}"
    input_file="${INPUT_FILES[$i]}"
    
    echo "========================================="
    echo "[$TEST_NUM/$TOTAL] $testname"
    echo "========================================="
    
    cat >> "$OUTPUT_FILE" << EOF

===========================================
Test $TEST_NUM: $testname
===========================================
Executable: $exe
Arguments: ${args:-none}
Working Directory: $workdir
Input File: ${input_file:-none}

EOF
    
    # Change to working directory
    cd "$workdir"
    
    # Build command with input redirection if needed
    if [ -n "$input_file" ] && [ -f "$input_file" ]; then
        cmd="$exe $args < $input_file"
        echo "Command: $cmd"
    else
        cmd="$exe $args"
        echo "Command: $cmd"
    fi
    
    # Profile
    echo ""
    echo "[1/5] Profiling..."
    
    # Run with perf (handle input redirection)
    if [ -n "$input_file" ] && [ -f "$input_file" ]; then
        if ! perf record -e cycles:u -o /tmp/perf_${testname}_$$.data \
             bash -c "$exe $args < $input_file" &>/tmp/perf_${testname}_out.txt; then
            echo "   Failed"
            echo "Status: FAILED" >> "$OUTPUT_FILE"
            cd - > /dev/null
            ((TEST_NUM++))
            continue
        fi
    else
        if ! perf record -e cycles:u -o /tmp/perf_${testname}_$$.data \
             $exe $args &>/tmp/perf_${testname}_out.txt; then
            echo "   Failed"
            echo "Status: FAILED" >> "$OUTPUT_FILE"
            cd - > /dev/null
            ((TEST_NUM++))
            continue
        fi
    fi
    echo "   Done"
    
    # Extract hotspots
    echo "[2/5] Extracting hotspots..."
    perf report -i /tmp/perf_${testname}_$$.data --stdio --no-children 2>/dev/null | \
        grep -E '^\s+[0-9]+\.[0-9]+%.*\[.\]' | \
        head -5 | \
        awk '{
            pct = $1
            for (i = 1; i <= NF; i++) {
                if ($i == "[.]") {
                    for (j = i + 1; j <= NF; j++)
                        printf "%s%s", $j, (j < NF ? " " : "")
                    printf "|%s\n", pct
                    break
                }
            }
        }' > /tmp/hotspots_${testname}_$$.txt
    
    if [ ! -s /tmp/hotspots_${testname}_$$.txt ]; then
        echo "   No hotspots"
        echo "Status: No hotspots" >> "$OUTPUT_FILE"
        rm -f /tmp/perf_${testname}_$$.data
        cd - > /dev/null
        ((TEST_NUM++))
        continue
    fi
    
    echo "   Found $(wc -l < /tmp/hotspots_${testname}_$$.txt)"
    
    # Display
    echo ""
    echo "Top 5 Hotspots:"
    echo "-----------------------------------"
    echo "Hotspots:" >> "$OUTPUT_FILE"
    rank=1
    while IFS='|' read -r func pct; do
        printf "  %d. %-8s %s\n" $rank "$pct" "$func"
        printf "  %d. %s (%s)\n" $rank "$func" "$pct" >> "$OUTPUT_FILE"
        ((rank++))
    done < /tmp/hotspots_${testname}_$$.txt
    echo "-----------------------------------"
    echo "" >> "$OUTPUT_FILE"
    
    # Add probes
    echo "[3/5] Adding probes..."
    PROBES=()
    while IFS='|' read -r func pct; do
        if perf probe -x "$exe" --add "$func" 2>/dev/null; then
            PROBES+=("$func")
            echo "   $func"
        fi
    done < /tmp/hotspots_${testname}_$$.txt
    
    if [ ${#PROBES[@]} -eq 0 ]; then
        echo "   No probes"
        echo "Call counts: Not available" >> "$OUTPUT_FILE"
        rm -f /tmp/perf_${testname}_$$.data /tmp/hotspots_${testname}_$$.txt
        cd - > /dev/null
        ((TEST_NUM++))
        continue
    fi
    
    # Get probe prefix
    PROBE_PREFIX=$(perf probe -l 2>/dev/null | sed -n 's/.*\(probe_[^:]*\):.*/\1/p' | head -1)
    
    if [ -z "$PROBE_PREFIX" ]; then
        for func in "${PROBES[@]}"; do
            perf probe -d "$func" 2>/dev/null
        done
        cd - > /dev/null
        ((TEST_NUM++))
        continue
    fi
    
    # Count calls
    echo "[4/5] Counting..."
    
    # Run with perf stat (handle input redirection)
    if [ -n "$input_file" ] && [ -f "$input_file" ]; then
        perf stat -e "${PROBE_PREFIX}:*" bash -c "$exe $args < $input_file" \
            2>&1 > /dev/null | tee /tmp/counts_${testname}_$$.txt | \
            grep -q "probe" && echo "   Done" || echo "   No data"
    else
        perf stat -e "${PROBE_PREFIX}:*" $exe $args \
            2>&1 > /dev/null | tee /tmp/counts_${testname}_$$.txt | \
            grep -q "probe" && echo "   Done" || echo "   No data"
    fi
    
    # Parse counts
    echo ""
    echo "Call Counts:"
    echo "-----------------------------------"
    echo "Call Counts:" >> "$OUTPUT_FILE"
    
    declare -A COUNTS
    while IFS= read -r line; do
        if echo "$line" | grep -qE '^\s+[0-9,]+\s+probe'; then
            count=$(echo "$line" | awk '{print $1}' | tr -d ',')
            func=$(echo "$line" | awk '{print $2}' | sed 's/probe[^:]*://')
            [ -n "$count" ] && [ -n "$func" ] && COUNTS["$func"]="$count"
        fi
    done < /tmp/counts_${testname}_$$.txt
    
    for func in "${!COUNTS[@]}"; do
        count="${COUNTS[$func]}"
        printf "  %-30s %12s\n" "$func" "$count"
        printf "  %-30s %12s\n" "$func" "$count" >> "$OUTPUT_FILE"
        ALL_HOTSPOTS+=("$func")
        ALL_COUNTS+=("$count")
    done | sort -k2 -nr
    echo "-----------------------------------"
    echo "" >> "$OUTPUT_FILE"
    
    # Cleanup
    echo "[5/5] Cleanup..."
    for func in "${PROBES[@]}"; do
        perf probe -d "$func" 2>/dev/null
    done
    rm -f /tmp/perf_${testname}_$$.data /tmp/hotspots_${testname}_$$.txt \
          /tmp/counts_${testname}_$$.txt /tmp/perf_${testname}_out.txt
    echo "   Done"
    
    cd - > /dev/null
    echo ""
    ((TEST_NUM++))
done

# Step 3: Generate annotations
echo "[3/4] Generating annotations..."

if [ ${#ALL_HOTSPOTS[@]} -gt 0 ]; then
    combined=()
    for i in "${!ALL_HOTSPOTS[@]}"; do
        combined+=("${ALL_COUNTS[$i]}|${ALL_HOTSPOTS[$i]}")
    done
    
    IFS=$'\n' sorted=($(printf '%s\n' "${combined[@]}" | sort -t'|' -k1 -nr))
    unset IFS
    
    declare -A seen
    for entry in "${sorted[@]}"; do
        count=$(echo "$entry" | cut -d'|' -f1)
        func=$(echo "$entry" | cut -d'|' -f2)
        
        if [ -z "${seen[$func]}" ]; then
            cat >> "$ANNOTATION_FILE" << EOF
// $func - ~$count calls
__attribute__((annotate("adaptive:$count")))

EOF
            seen[$func]=1
        fi
    done
    
    echo "   ${#seen[@]} annotations"
fi

# Step 4: Summary
echo ""
echo "[4/4] Summary"
echo "========================================="
echo "Tests found: ${#TEST_NAMES[@]}"
echo "Tests profiled: $((TEST_NUM - 1))"
echo ""

if [ ${#ALL_HOTSPOTS[@]} -gt 0 ]; then
    unique=$(printf '%s\n' "${ALL_HOTSPOTS[@]}" | sort -u | wc -l)
    echo "Statistics:"
    echo "  Unique functions: $unique"
    echo "  Total hotspots: ${#ALL_HOTSPOTS[@]}"
    echo ""
fi

echo "Output files:"
echo "  • $OUTPUT_FILE"
echo "  • $ANNOTATION_FILE"
echo ""

# Show configuration used
echo "Test configurations:"
echo "-----------------------------------"
for i in "${!TEST_NAMES[@]}"; do
    printf "  %-15s" "${TEST_NAMES[$i]}"
    [ -n "${TEST_ARGS[$i]}" ] && printf " : %s" "${TEST_ARGS[$i]}"
    [ -n "${INPUT_FILES[$i]}" ] && printf " < %s" "$(basename "${INPUT_FILES[$i]}")"
    echo ""
done
echo "-----------------------------------"
echo ""

echo " Complete!"