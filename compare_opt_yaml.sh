#!/bin/bash
# folder paths
dir1=$1   # first folder
dir2=$2   # second folder

# Extract base names from first folder
find "$dir1" -type f -name '*.yaml' \
  | sed -E 's/\.opt\.yaml$//' \
  | sed -E 's/\.diff(\.[0-9]+)?$/.diff/' \
  | xargs -n1 basename \
  | sort -u > /tmp/list1.txt

# Extract base names from second folder
find "$dir2" -type f -name '*.yaml' \
  | sed -E 's/\.opt\.yaml$//' \
  | sed -E 's/\.diff(\.[0-9]+)?$/.diff/' \
  | xargs -n1 basename \
  | sort -u > /tmp/list2.txt

# Compare
echo "Only in $dir1:"
comm -23 /tmp/list1.txt /tmp/list2.txt

echo "Only in $dir2:"
comm -13 /tmp/list1.txt /tmp/list2.txt
