import re, glob
import sys

thresholds = []
filename = sys.argv[1]

with open(filename) as f:
    for line in f:
        #print (line)
        m = re.search(r"Threshold:\s*'(-?\d+)'", line)
        if m:
            #print (m)
            thresholds.append(int(m.group(1)))
if thresholds:
    print("Average threshold:", sum(thresholds) / len(thresholds))
else:
    print("No thresholds found")
