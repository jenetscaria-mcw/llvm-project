#!/usr/bin/env python

from __future__ import print_function

desc = """yaml opt diff"""

import yaml

# Try to use the C parser.
try:
    from yaml import CLoader as Loader
except ImportError:
    from yaml import Loader

import optrecord
import argparse
from collections import defaultdict
import os
import os.path

def get_relative_path(file_path, base_dir):
    if os.path.isfile(base_dir):
        # If base_dir is a file, just return the filename
        return os.path.basename(file_path)
    return os.path.relpath(file_path, base_dir)

def match_files(files1, dir1, files2, dir2):
    # Create a mapping of relative paths to full paths
    files1_map = {}
    files2_map = {}
    
    for f in files1:
        rel_path = get_relative_path(f, dir1)
        files1_map[rel_path] = f
    
    for f in files2:
        rel_path = get_relative_path(f, dir2)
        files2_map[rel_path] = f
    
    # Find common files and unique files
    common_files = []
    for rel_path in files1_map:
        if rel_path in files2_map:
            common_files.append((rel_path, files1_map[rel_path], files2_map[rel_path]))
    
    # Files only in first directory
    unique_to_first = []
    for rel_path in files1_map:
        if rel_path not in files2_map:
            unique_to_first.append((rel_path, files1_map[rel_path]))
    
    # Files only in second directory
    unique_to_second = []
    for rel_path in files2_map:
        if rel_path not in files1_map:
            unique_to_second.append((rel_path, files2_map[rel_path]))
    
    return common_files, unique_to_first, unique_to_second

def process_file_pair(file1, file2, args, print_progress):
    # Get remarks from both files
    max_hotness1, all_remarks1, _ = optrecord.get_remarks(file1)
    max_hotness2, all_remarks2, _ = optrecord.get_remarks(file2)
    
    # Compute differences
    added = set(all_remarks2.values()) - set(all_remarks1.values())
    removed = set(all_remarks1.values()) - set(all_remarks2.values())
    
    # Mark added and removed remarks
    for r in added:
        r.Added = True
    for r in removed:
        r.Added = False
    

    result = list(added | removed)

    result.sort(
        key=lambda r: (
            r.PassWithDiffPrefix,
            r.Name,
            r.File,
            r.Line,
            r.Column,
            r.Function,
        )
    )

    for r in result:
        r.recover_yaml_structure()
    
    
    return result

def generate_output_filename(rel_path, output_pattern):
    # Remove .opt.yaml extension and add .diff before .opt.yaml
    base_name = rel_path
    if base_name.endswith('.opt.yaml'):
        base_name = base_name[:-9]  # Remove .opt.yaml
    elif base_name.endswith('.yaml'):
        base_name = base_name[:-5]  # Remove .yaml
    
    # Replace directory separators with underscores to create a flat structure
    # or maintain directory structure based on preference
    flat_name = base_name.replace(os.sep, '_')
    
    return output_pattern.format(flat_name)

def write_output(result, output_file, max_size):
    if len(result) == 0:
        # Don't create empty files
        return
    #print (type(result))

    #max_size = 10
    if len(result) <= max_size:
        # Write all results to a single file
        with open(output_file, 'w') as stream:
            yaml.dump_all(result, stream)
        print(f"  Wrote {len(result)} remarks to {output_file}")
    else:
        # Split into multiple files
        for i in range(0, len(result), max_size):
            chunk_idx = i // max_size
            chunk_file = output_file.replace('.opt.yaml', f'.{chunk_idx}.opt.yaml')
            with open(chunk_file, 'w') as stream:
                yaml.dump_all(result[i:i + max_size], stream)
            print(f"  Wrote {len(result[i:i + max_size])} remarks to {chunk_file}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=desc)
    parser.add_argument(
        "yaml_dir_or_file_1",
        help="An optimization record file or a directory searched for optimization "
        "record files that are used as the old version for the comparison",
    )
    parser.add_argument(
        "yaml_dir_or_file_2",
        help="An optimization record file or a directory searched for optimization "
        "record files that are used as the new version for the comparison",
    )
    parser.add_argument(
        "--jobs",
        "-j",
        default=None,
        type=int,
        help="Max job count (defaults to %(default)s, the current CPU count)",
    )
    parser.add_argument(
        "--max-size",
        "-m",
        default=10000,
        type=int,
        help="Maximum number of remarks stored in an output file",
    )
    parser.add_argument(
        "--no-progress-indicator",
        "-n",
        action="store_true",
        default=False,
        help="Do not display any indicator of how many YAML files were read.",
    )
    parser.add_argument(
        "--output", 
        "-o", 
        default="{}.diff.opt.yaml",
        help="Output file pattern. Use {} as placeholder for the base filename. Default: %(default)s"
    )
    parser.add_argument(
        "--output-dir",
        "-d",
        default=".",
        help="Output directory for difference files. Default: current directory"
    )
    parser.add_argument(
        "--preserve-structure",
        "-p",
        action="store_true",
        default=False,
        help="Preserve directory structure in output directory"
    )
    args = parser.parse_args()

    print_progress = not args.no_progress_indicator
    
    # Create output directory if it doesn't exist
    if not os.path.exists(args.output_dir):
        os.makedirs(args.output_dir)
    
    # Check if inputs are files or directories
    is_file1 = os.path.isfile(args.yaml_dir_or_file_1)
    is_file2 = os.path.isfile(args.yaml_dir_or_file_2)
    
    if is_file1 and is_file2:
        # Both are files - simple comparison
        if print_progress:
            print(f"Comparing single file pair...")
            print(f"  File 1: {args.yaml_dir_or_file_1}")
            print(f"  File 2: {args.yaml_dir_or_file_2}")
        
        result = process_file_pair(args.yaml_dir_or_file_1, args.yaml_dir_or_file_2, 
                                   args, print_progress)
        
        base_name = os.path.basename(args.yaml_dir_or_file_1)
        if base_name.endswith('.opt.yaml'):
            base_name = base_name[:-9]
        elif base_name.endswith('.yaml'):
            base_name = base_name[:-5]
        
        output_file = os.path.join(args.output_dir, args.output.format(base_name))
        write_output(result, output_file, args.max_size)
        
    else:
        # At least one is a directory - find all files
        files1 = optrecord.find_opt_files(args.yaml_dir_or_file_1)
        files2 = optrecord.find_opt_files(args.yaml_dir_or_file_2)
        
        if not files1:
            parser.error(f"No *.opt.yaml files found in {args.yaml_dir_or_file_1}")
        if not files2:
            parser.error(f"No *.opt.yaml files found in {args.yaml_dir_or_file_2}")
        
        if print_progress:
            print(f"Found {len(files1)} files in first location")
            print(f"Found {len(files2)} files in second location")
        
        # Match files between directories
        common_files, unique_to_first, unique_to_second = match_files(
            files1, args.yaml_dir_or_file_1, 
            files2, args.yaml_dir_or_file_2
        )
        
        if print_progress:
            print(f"\nMatched {len(common_files)} common files")
            if unique_to_first:
                print(f"Files only in first location: {len(unique_to_first)}")
            if unique_to_second:
                print(f"Files only in second location: {len(unique_to_second)}")
            print()
        
        # Process common files
        common_files_count = 0
        for rel_path, file1, file2 in common_files:
            if print_progress:
                print(f"Processing {common_files_count} : {rel_path}")
            common_files_count = common_files_count + 1
            result = process_file_pair(file1, file2, args, print_progress)
            
            if args.preserve_structure:
                # Preserve directory structure
                output_path = os.path.join(args.output_dir, rel_path)
                output_dir = os.path.dirname(output_path)
                if not os.path.exists(output_dir):
                    os.makedirs(output_dir)
                
                # Generate output filename
                base_name = os.path.basename(rel_path)
                if base_name.endswith('.opt.yaml'):
                    base_name = base_name[:-9]
                elif base_name.endswith('.yaml'):
                    base_name = base_name[:-5]
                output_file = os.path.join(output_dir, args.output.format(base_name))
            else:
                # Flatten structure
                output_file = os.path.join(args.output_dir, 
                                         generate_output_filename(rel_path, args.output))
            
            write_output(result, output_file, args.max_size)
        
        # Process files unique to first location (all remarks are "removed")
        if unique_to_first and print_progress:
            print(f"\nProcessing files only in first location (all remarks marked as removed):")
        
        unique_to_first_count = 0
        for rel_path, file1 in unique_to_first:
            if print_progress:
                print(f"Processing (removed) { unique_to_first_count } : {rel_path}")
            unique_to_first_count = unique_to_first_count + 1
            max_hotness1, all_remarks1, _ = optrecord.get_remarks(file1)
            
            result = list(all_remarks1.values())
            for r in result:
                r.Added = False
                r.recover_yaml_structure()
            
            if args.preserve_structure:
                output_path = os.path.join(args.output_dir, rel_path)
                output_dir = os.path.dirname(output_path)
                if not os.path.exists(output_dir):
                    os.makedirs(output_dir)
                
                base_name = os.path.basename(rel_path)
                if base_name.endswith('.opt.yaml'):
                    base_name = base_name[:-9]
                elif base_name.endswith('.yaml'):
                    base_name = base_name[:-5]
                output_file = os.path.join(output_dir, args.output.format(base_name))
            else:
                output_file = os.path.join(args.output_dir, 
                                         generate_output_filename(rel_path, args.output))
            
            write_output(result, output_file, args.max_size)
        
        # Process files unique to second location (all remarks are "added")
        if unique_to_second and print_progress:
            print(f"\nProcessing files only in second location (all remarks marked as added):")
        
        unique_to_second_count = 0
        for rel_path, file2 in unique_to_second:
            if print_progress:
                print(f"Processing (added) {unique_to_second_count} : {rel_path}")
            unique_to_second_count = unique_to_second_count + 1
            max_hotness2, all_remarks2, _ = optrecord.get_remarks(file2)
            
            result = list(all_remarks2.values())
            for r in result:
                r.Added = True
                r.recover_yaml_structure()
            
            if args.preserve_structure:
                output_path = os.path.join(args.output_dir, rel_path)
                output_dir = os.path.dirname(output_path)
                if not os.path.exists(output_dir):
                    os.makedirs(output_dir)
                
                base_name = os.path.basename(rel_path)
                if base_name.endswith('.opt.yaml'):
                    base_name = base_name[:-9]
                elif base_name.endswith('.yaml'):
                    base_name = base_name[:-5]
                output_file = os.path.join(output_dir, args.output.format(base_name))
            else:
                output_file = os.path.join(args.output_dir, 
                                         generate_output_filename(rel_path, args.output))
            
            write_output(result, output_file, args.max_size)
        
        if print_progress:
            print(f"\nDifference generation complete!")
            print(f"Output files written to: {args.output_dir}")