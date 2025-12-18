#!/bin/bash

set -e

# Check arguments
if [ $# -lt 1 ]; then
    echo "Usage: $0 <executable> [args...]"
    echo "Example: $0 ./7zip-benchmark b"
    exit 1
fi

EXECUTABLE="$1"
shift
ARGS="$@"

# Validate executable
if [ ! -x "$EXECUTABLE" ]; then
    echo "Error: '$EXECUTABLE' not found or not executable"
    exit 1
fi

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Rerunning with sudo..."
    exec sudo "$0" "$EXECUTABLE" $ARGS
fi

EXECUTABLE=$(realpath "$EXECUTABLE")

echo "========================================="
echo "Profiling: $EXECUTABLE $ARGS"
echo "========================================="
echo ""

# Step 1: Run perf to profile
echo "[1/5] Running perf record..."
perf record -e cycles:u -o /tmp/perf_$$.data "$EXECUTABLE" $ARGS &>/dev/null
echo " Profiling complete"
echo ""

# Step 2: Get top 5 hotspot function names
echo "[2/5] Extracting top 5 hotspots..."
perf report -i /tmp/perf_$$.data --stdio --no-children 2>/dev/null | \
    grep -E '^\s+[0-9]+\.[0-9]+%.*\[.\]' | \
    awk '{
        for(i=1; i<=NF; i++) {
            if($i == "[.]" && i < NF) {
                print $(i+1)
                break
            }
        }
    }' | \
    head -5 > /tmp/hotspots_$$.txt

if [ ! -s /tmp/hotspots_$$.txt ]; then
    echo " No hotspot functions found"
    rm -f /tmp/perf_$$.data
    exit 1
fi

echo " Found hotspots:"
cat /tmp/hotspots_$$.txt | nl -w2 -s'. '
echo ""

# Step 3: Add uprobe for each hotspot
echo "[3/5] Adding uprobes..."
PROBES=()
while IFS= read -r func; do
    if perf probe -x "$EXECUTABLE" --add "$func" 2>/dev/null; then
        PROBES+=("$func")
        echo "   $func"
    else
        echo "   $func (failed)"
    fi
done < /tmp/hotspots_$$.txt

if [ ${#PROBES[@]} -eq 0 ]; then
    echo " No probes could be added"
    rm -f /tmp/perf_$$.data /tmp/hotspots_$$.txt
    exit 1
fi
echo ""

# Find probe prefix from current probes (e.g. probe_7zip from probe_7zip:LzmaDec_DecodeReal)
PROBE_PREFIX=$(
    perf probe -l 2>/dev/null \
    | sed -n 's/.*\(probe_[^:]*\):.*/\1/p' \
    | head -1
)

if [ -z "$PROBE_PREFIX" ]; then
    echo " Could not determine probe prefix (run 'perf probe -l' to inspect)"
    exit 1
fi

EVENTS="${PROBE_PREFIX}:*"

echo "[4/5] Counting function calls with events: $EVENTS"
perf stat -e "$EVENTS" "$EXECUTABLE" $ARGS 2>&1 | tee /tmp/counts_$$.txt

# Parse and display counts
echo "Call Count Summary:"
echo "-----------------------------------"
declare -A COUNTS
while IFS= read -r line; do
    if echo "$line" | grep -qE '^\s+[0-9,]+\s+probe:'; then
        count=$(echo "$line" | awk '{print $1}' | tr -d ',')
        func=$(echo "$line" | awk '{print $2}' | sed 's/probe[^:]*://')
        [ ! -z "$count" ] && [ ! -z "$func" ] && COUNTS["$func"]="$count"
    fi
done < /tmp/counts_$$.txt

for func in "${!COUNTS[@]}"; do
    printf "%-30s %10d calls\n" "$func" "${COUNTS[$func]}"
done | sort -k2 -nr
echo "-----------------------------------"
echo ""

# Step 5: Delete uprobes
echo "[5/5] Cleaning up uprobes..."
for func in "${PROBES[@]}"; do
    perf probe -d "$func" 2>/dev/null && echo "   Removed $func"
done
echo ""

# Cleanup temp files
rm -f /tmp/perf_$$.data /tmp/hotspots_$$.txt /tmp/counts_$$.txt

echo " Done!"