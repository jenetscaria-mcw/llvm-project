import sys
import yaml
import glob
import os
from collections import defaultdict
from multiprocessing import Pool, cpu_count
from functools import partial
import time

# --- Fix: make PyYAML ignore custom tags like !Passed / !Missed ---
class NoTagLoader(yaml.SafeLoader):
    pass

def unknown_tag_handler(loader, tag_suffix, node):
    data = loader.construct_mapping(node)
    data["__tag__"] = tag_suffix  # preserve the tag name (!Passed, !Missed, etc.)
    return data

NoTagLoader.add_multi_constructor('!', unknown_tag_handler)

def load_yaml_entries(filename):
    """Load multi-document YAML (LLVM opt remarks)"""
    with open(filename, 'r') as f:
        docs = list(yaml.load_all(f, Loader=NoTagLoader))
    return docs

def extract_caller_callee_info(entry, pass_name):
    """Extract caller, callee and tag from entry"""
    pass_name_entry = entry.get("Pass", "Unknown")
    if pass_name not in pass_name_entry:
        return None

    tag = entry.get("__tag__", "Unknown")
    callee = None
    caller = None

    if "Args" in entry:
        for arg in entry["Args"]:
            if "Caller" in arg:
                caller = arg["Caller"]
                break
    
    if "Args" in entry:
        for arg in entry["Args"]:
            if "Callee" in arg:
                callee = arg["Callee"]
                break
    
    return caller, callee, tag

def group_by_callee_with_callers(entries, pass_name):
    """Group by callee, storing (caller, tag) pairs"""
    grouped = defaultdict(list)
    
    for e in entries:
        res = extract_caller_callee_info(e, pass_name)
        if res is not None:
            caller, callee, tag = res
            if callee:
                grouped[callee].append((caller, tag))
        else:
            caller, callee, tag = None, None, None
    
    return grouped

def process_single_file(args):
    """Process a single YAML file and return counts"""
    filename, pass_name, file_index = args
    
    print(f"Processing {file_index}: {filename}")
    
    try:
        entries = load_yaml_entries(filename)
        grouped = group_by_callee_with_callers(entries, pass_name)
        
        local_func_count = [0, 0]  # [mixed_count, same_count]
        
        # Process grouped data
        for callee, caller_tag_pairs in grouped.items():
            values = [[caller, tag] for caller, tag in caller_tag_pairs]
            output = defaultdict(list)
            for value in values:
                output[value[0]].append(value[1])
            
            for key, value in output.items():
                if "Analysis" in set(value):
                    print(f"Found 'Analysis' in {filename}. It considers. Modify the logic")
                    return None  # Signal that we should exit
                
                if len(set(value)) != 1:
                    local_func_count[0] += 1
                else:
                    local_func_count[1] += 1
        
        return local_func_count
    
    except Exception as e:
        print(f"Error processing {filename}: {str(e)}")
        return [0, 0]

def find_opt_files(dirs_or_files):
    """Find all .opt.yaml files recursively"""
    all_files = glob.iglob(os.path.join(dirs_or_files, "**", "*.opt.yaml"), recursive=True)
    return list(all_files)  # Convert to list for multiprocessing

def main_parallel(yaml_dir, pass_name, num_processes=None):
    """Main function with parallel processing"""
    # Find all files
    all_files = find_opt_files(yaml_dir)
    
    if not all_files:
        print(f"No .opt.yaml files found in {yaml_dir}")
        return [0, 0]
    
    print(f"Found {len(all_files)} YAML files to process")
    
    # Determine number of processes
    if num_processes is None:
        num_processes = min(cpu_count(), len(all_files))
    
    print(f"Using {num_processes} processes for parallel execution")
    
    # Prepare arguments for each file
    file_args = [(f, pass_name, i) for i, f in enumerate(all_files)]
    
    # Process files in parallel
    start_time = time.time()
    
    with Pool(processes=num_processes) as pool:
        results = pool.map(process_single_file, file_args)
    
    # Check if any file triggered the exit condition
    if None in results:
        print("Exit condition met - 'Analysis' found in tags")
        sys.exit(0)
    
    # Aggregate results
    total_func_count = [0, 0]
    for result in results:
        total_func_count[0] += result[0]
        total_func_count[1] += result[1]
    
    end_time = time.time()
    print(f"\nProcessing completed in {end_time - start_time:.2f} seconds")
    
    return total_func_count

def main_sequential(yaml_dir, pass_name):
    """Original sequential processing for comparison"""
    all_files = find_opt_files(yaml_dir)
    func_count = [0, 0]
    
    start_time = time.time()
    
    for i, filename in enumerate(all_files):
        print(f"Processing {i}: {filename}")
        
        try:
            entries = load_yaml_entries(filename)
            grouped = group_by_callee_with_callers(entries, pass_name)
            
            for callee, caller_tag_pairs in grouped.items():
                values = [[caller, tag] for caller, tag in caller_tag_pairs]
                output = defaultdict(list)
                for value in values:
                    output[value[0]].append(value[1])
                
                for key, value in output.items():
                    if "Analysis" in set(value):
                        print("It considers. Modify the logic")
                        sys.exit(0)
                    
                    if len(set(value)) != 1:
                        func_count[0] += 1
                    else:
                        func_count[1] += 1
        
        except Exception as e:
            print(f"Error processing {filename}: {str(e)}")
    
    end_time = time.time()
    print(f"\nSequential processing completed in {end_time - start_time:.2f} seconds")
    
    return func_count

if __name__ == "__main__":
    if len(sys.argv) < 3 or len(sys.argv) > 5:
        print("Usage: python3 find_same_and_mixed_case_count_in_pass.py <yaml_dir> <pass_name> [--parallel] [--processes N]")
        print("  --parallel: Use parallel processing (default)")
        print("  --sequential: Use sequential processing")
        print("  --processes N: Number of processes to use (default: number of CPU cores)")
        sys.exit(1)
    
    yaml_dir = sys.argv[1]
    pass_name = sys.argv[2]
    
    # Parse optional arguments
    use_parallel = True
    num_processes = None
    
    for i in range(3, len(sys.argv)):
        if sys.argv[i] == "--sequential":
            use_parallel = False
        elif sys.argv[i] == "--parallel":
            use_parallel = True
        elif sys.argv[i] == "--processes" and i + 1 < len(sys.argv):
            try:
                num_processes = int(sys.argv[i + 1])
            except ValueError:
                print(f"Invalid number of processes: {sys.argv[i + 1]}")
                sys.exit(1)
    
    # Run the appropriate version
    if use_parallel:
        print("Running parallel version...")
        func_count = main_parallel(yaml_dir, pass_name, num_processes)
    else:
        print("Running sequential version...")
        func_count = main_sequential(yaml_dir, pass_name)
    
    print(f"\nFinal counts: {func_count}")
    print(f"Mixed cases: {func_count[0]}")
    print(f"Same cases: {func_count[1]}")