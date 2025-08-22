#!/usr/bin/env python

from __future__ import print_function

desc = """Generate the difference of two YAML files into a new YAML file (works on
pair of directories too).  A new attribute 'Added' is set to True or False
depending whether the entry is added or removed from the first input to the
next.

The tools requires PyYAML."""

import yaml

# Try to use the C parser.
try:
    from yaml import CLoader as Loader
except ImportError:
    from yaml import Loader

import optrecord
import argparse
from collections import defaultdict

def get_remark_key(r):
    """
    Create a hashable key from a remark object based on its content.
    """
    # Convert the Args list of tuples to a hashable tuple.
    # Use `r.Args or []` to handle cases where Args might be None.
    frozen_args = tuple(r.Args or [])

    # The traceback shows that r.DebugLoc is a dictionary, not an object.
    # Access elements using dictionary keys. Use .get() for safety.
    loc = (None, None, None)
    if r.DebugLoc:
        loc = (r.DebugLoc.get('File'),
               r.DebugLoc.get('Line'),
               r.DebugLoc.get('Column'))

    return (r.Pass, r.Name, r.Function, loc, frozen_args)

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
        default=100000,
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
    parser.add_argument("--output", "-o", default="diff{}.opt.yaml")
    args = parser.parse_args()

    files1 = optrecord.find_opt_files(args.yaml_dir_or_file_1)
    files2 = optrecord.find_opt_files(args.yaml_dir_or_file_2)

    print_progress = not args.no_progress_indicator
    all_remarks1, _, _ = optrecord.gather_results(files1, args.jobs, print_progress)
    all_remarks2, _, _ = optrecord.gather_results(files2, args.jobs, print_progress)

    # Create dictionaries mapping the content-based key to each remark object.
    remarks1_map = {get_remark_key(r): r for r in all_remarks1.values()}
    remarks2_map = {get_remark_key(r): r for r in all_remarks2.values()}

    # Use the keys to find the added and removed remarks.
    added_keys = set(remarks2_map.keys()) - set(remarks1_map.keys())
    removed_keys = set(remarks1_map.keys()) - set(remarks2_map.keys())

    added = [remarks2_map[key] for key in added_keys]
    removed = [remarks1_map[key] for key in removed_keys]

    for r in added:
        r.Added = True
    for r in removed:
        r.Added = False

    result = list(added) + list(removed)
    for r in result:
        r.recover_yaml_structure()

    # for i in range(0, len(result), args.max_size):
    #     output_filename = args.output.format("" if i == 0 else i // args.max_size)
    #     with open(output_filename, "w") as stream:
    #         yaml.dump_all(result[i : i + args.max_size], stream)

    output_filename = args.output
    with open(output_filename, "w") as stream:
        yaml.dump_all(result, stream)
