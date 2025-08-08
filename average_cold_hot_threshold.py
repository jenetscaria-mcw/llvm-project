import re
import sys

file_path = sys.argv[1]

# Patterns to detect threshold and hotness
threshold_pattern = re.compile(r"Threshold:\s*'(-?\d+)'")
hotness_pattern = re.compile(r"Hotness:\s*(\d+)")

hot_thresholds = []
cold_thresholds = []

current_hotness = None

with open(file_path, "r", encoding="utf-8") as f:
    for line in f:
        # Detect hotness value
        hot_match = hotness_pattern.search(line)
        if hot_match:
            current_hotness = int(hot_match.group(1))
            continue

        # Detect threshold value
        thr_match = threshold_pattern.search(line)
        if thr_match and current_hotness is not None:
            thr_value = int(thr_match.group(1))
            if current_hotness > 0:
                hot_thresholds.append(thr_value)
            else:
                cold_thresholds.append(thr_value)

# Results
if hot_thresholds:
    print(f"Hot sites: {len(hot_thresholds)} | Average threshold: {sum(hot_thresholds)/len(hot_thresholds):.2f}")
else:
    print("No hot site thresholds found.")

if cold_thresholds:
    print(f"Cold sites: {len(cold_thresholds)} | Average threshold: {sum(cold_thresholds)/len(cold_thresholds):.2f}")
else:
    print("No cold site thresholds found.")
