#!/bin/bash

# Search for all *.diff.0.opt.yaml files in current dir and subdirs
find $1 -type f -name "*.diff.0.opt.yaml" | while read f; do
    echo "Processing $f"
    dir=$(dirname "$f")                       # subdirectory path
    base=$(basename "$f" .0.opt.yaml)         # strip the .0.opt.yaml
    out="$dir/${base}.opt.yaml"               # output file in same dir

    # Remove old output if exists
    rm -f "$out"

    chunks=$(ls "$dir"/"$base".[0-9]*.opt.yaml | sort -V)

    first=true
    for chunk in $chunks; do
        if $first; then
            cat "$chunk" >> "$out"
            first=false
        else
            echo -n "--- " >> "$out"
            cat "$chunk" >> "$out"
        fi
    done

    echo "Combined into $out"

    # Delete the old chunks
    rm -f $chunks
done
