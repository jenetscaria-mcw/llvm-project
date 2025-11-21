#!/usr/bin/env python3

import re
import argparse
import sys
from pathlib import Path


def parse_build_command(line):
    # Match pattern like [123/5017] at the start
    match = re.match(r'\[(\d+)/(\d+)\]\s+(.*)', line.strip())
    if not match:
        return None
    
    build_num = int(match.group(1))
    total = int(match.group(2))
    command = match.group(3)
    
    return build_num, total, command


def clean_command(command):
    # Replace full paths to clang++ with just clang++
    command = re.sub(r'\S+/clang\+\+', 'clang++', command)
    # Replace full paths to clang with just clang
    command = re.sub(r'\S+/clang(?!\+)', 'clang', command)
    
    return command


def process_build_log(input_file, output_file, build_numbers=None):
    commands = []
    
    try:
        with open(input_file, 'r') as f:
            for line in f:
                result = parse_build_command(line)
                if result:
                    build_num, total, command = result
                    
                    # Filter by build numbers if specified
                    if build_numbers is None or build_num in build_numbers:
                        cleaned_cmd = clean_command(command)
                        commands.append((build_num, cleaned_cmd))
        
        # Sort by build number
        commands.sort(key=lambda x: x[0])
        
        # Write to output file
        with open(output_file, 'w') as f:
            f.write("#!/bin/bash\n")
            f.write("# Generated from build log\n")
            f.write(f"# Total commands: {len(commands)}\n\n")
            
            for build_num, command in commands:
                f.write(f"# Build {build_num}\n")
                f.write(f"{command}\n\n")
        
        print(f"✓ Successfully generated {output_file}")
        print(f"  Extracted {len(commands)} command(s)")
        
    except FileNotFoundError:
        print(f"Error: Input file '{input_file}' not found", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


def parse_build_numbers(numbers_str):
    if not numbers_str:
        return None
    
    numbers = set()
    parts = numbers_str.split(',')
    
    for part in parts:
        part = part.strip()
        if '-' in part:
            # Range like "100-105"
            try:
                start, end = map(int, part.split('-'))
                numbers.update(range(start, end + 1))
            except ValueError:
                print(f"Warning: Invalid range '{part}', skipping", file=sys.stderr)
        else:
            # Single number
            try:
                numbers.add(int(part))
            except ValueError:
                print(f"Warning: Invalid number '{part}', skipping", file=sys.stderr)
    
    return sorted(numbers) if numbers else None


def main():
    parser = argparse.ArgumentParser(
        description='Parse build log and generate shell script',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
        """)
    
    parser.add_argument('input', help='Input build log file')
    parser.add_argument('-o', '--output', required=True, 
                       help='Output shell script file')
    parser.add_argument('-n', '--numbers', 
                       help='Specific build numbers to extract (e.g., "100,200,300" or "100-110")')
    
    args = parser.parse_args()
    
    # Parse build numbers
    build_numbers = parse_build_numbers(args.numbers)
    
    if build_numbers:
        print(f"Extracting build numbers: {build_numbers}")
    else:
        print("Extracting all build commands")
    
    # Process the file
    process_build_log(args.input, args.output, build_numbers)
    
    # Make output file executable
    try:
        Path(args.output).chmod(0o755)
        print(f"Made {args.output} executable")
    except Exception as e:
        print(f"Warning: Could not make file executable: {e}", file=sys.stderr)


if __name__ == '__main__':
    main()
