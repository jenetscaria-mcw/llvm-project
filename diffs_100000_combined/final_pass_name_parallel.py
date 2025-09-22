# {Pass: {"Passed": [Names], "Missed": [Names]}}
import sys
import yaml
import os
from collections import defaultdict
from pathlib import Path
import multiprocessing as mp
from functools import partial

class NoTagLoader(yaml.SafeLoader):
    pass

# Capture tag type (!Passed / !Missed / !Analysis / etc.)
def unknown_tag_handler(loader, tag_suffix, node):
    data = loader.construct_mapping(node)
    data["_tag"] = tag_suffix.lstrip("!")  # store tag type
    return data

NoTagLoader.add_multi_constructor("!", unknown_tag_handler)

def parse_single_yaml_file(args):
    """Parse a single YAML file and return its results"""
    file_path, file_index, total_files = args
    pass_to_results = defaultdict(lambda: defaultdict(set))
    
    try:
        print(f"Processing file {file_index}/{total_files}: {os.path.basename(file_path)}")
        
        with open(file_path, "r") as f:
            docs = yaml.load_all(f, Loader=NoTagLoader)

            for doc in docs:
                if not doc:
                    continue
                llvm_pass = doc.get("Pass")
                name = doc.get("Name")
                tag_type = doc.get("_tag")  # Passed, Missed, Analysis, etc.
                if llvm_pass and name and tag_type:
                    pass_to_results[llvm_pass][tag_type].add(name)

        # Convert nested defaultdict to regular dict with sorted lists
        result = {}
        for pass_name, tag_dict in pass_to_results.items():
            result[pass_name] = {
                tag_type: sorted(list(names_set)) 
                for tag_type, names_set in tag_dict.items()
            }
        
        print(f"Completed file {file_index}/{total_files}: {os.path.basename(file_path)} - Found {len(result)} passes")
        return result, file_path, None
        
    except Exception as e:
        print(f"Error processing file {file_index}/{total_files}: {os.path.basename(file_path)} - {str(e)}")
        return {}, file_path, str(e)

def find_yaml_files(directory):
    """Find all YAML files in directory and subdirectories"""
    yaml_files = []
    directory_path = Path(directory)
    
    # Find all .yaml and .yml files recursively
    for ext in ["*.yaml", "*.yml"]:
        yaml_files.extend(directory_path.rglob(ext))
    
    return [str(f) for f in yaml_files]

def merge_results(all_results):
    """Merge results from all files"""
    merged_result = defaultdict(lambda: defaultdict(set))
    successful_files = 0
    failed_files = 0
    
    for file_result, file_path, error in all_results:
        if error:
            print(f"Error in results for {os.path.basename(file_path)}: {error}")
            failed_files += 1
            continue
            
        successful_files += 1
        for pass_name, tag_dict in file_result.items():
            for tag_type, names_list in tag_dict.items():
                merged_result[pass_name][tag_type].update(names_list)
    
    print(f"\nSuccessfully processed: {successful_files} files")
    print(f"Failed to process: {failed_files} files")
    
    # Convert to regular dict with sorted lists
    final_result = {}
    for pass_name, tag_dict in merged_result.items():
        final_result[pass_name] = {
            tag_type: sorted(list(names_set)) 
            for tag_type, names_set in tag_dict.items()
        }
    
    return final_result

def parse_yaml_directory(directory, num_processes=None):
    """Parse all YAML files in directory using multiprocessing"""
    if num_processes is None:
        num_processes = mp.cpu_count()
    
    # Find all YAML files
    yaml_files = find_yaml_files(directory)
    
    if not yaml_files:
        print(f"No YAML files found in directory: {directory}")
        return {}, 0
    
    print(f"Found {len(yaml_files)} YAML files to process")
    print(f"Using {num_processes} processes")
    print("-" * 60)
    
    # Prepare file arguments with indices
    file_args = [(file_path, idx + 1, len(yaml_files)) 
                for idx, file_path in enumerate(yaml_files)]
    
    # Create pool and process files in parallel
    with mp.Pool(processes=num_processes) as pool:
        all_results = pool.map(parse_single_yaml_file, file_args)
    
    print("-" * 60)
    # Merge results from all files
    final_result = merge_results(all_results)
    
    return final_result, len(yaml_files)

def main(yaml_dir, num_processes=None):
    result, files_processed = parse_yaml_directory(yaml_dir, num_processes)

    print(f"\nFinal Summary:")
    print(f"Processed {files_processed} YAML files from directory: {yaml_dir}")
    print(f"Total unique passes found: {len(result)}")
    
    # Print summary statistics
    total_names = 0
    for k, v in result.items():
        pass_total = sum(len(names) for names in v.values())
        total_names += pass_total
        print(f"{k}: {v} (total names: {pass_total})")
    
    print(f"\nTotal unique names across all passes: {total_names}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 collect_pass_name_dict.py <yaml_directory> [num_processes]")
        print("Example: python3 collect_pass_name_dict.py /path/to/yaml/files 8")
        sys.exit(1)

    yaml_dir = sys.argv[1]
    
    # Optional: number of processes
    num_processes = None
    if len(sys.argv) >= 3:
        try:
            num_processes = int(sys.argv[2])
        except ValueError:
            print("Warning: Invalid number of processes, using default")
    
    # Set multiprocessing start method for better compatibility
    mp.set_start_method('spawn', force=True)
    
    main(yaml_dir, num_processes)