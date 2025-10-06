import sys
import yaml
import glob
import os
from collections import defaultdict

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
    #print (pass_name_entry, pass_name)
    
    added_entry = entry.get("Added", "Unknown")
    if not added_entry:
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
    #print (caller, callee, tag)
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

def main(filename, pass_name, func_count, fp_pass, fp_miss):
    entries = load_yaml_entries(filename)
    grouped = group_by_callee_with_callers(entries, pass_name)
    pass_count = 0
    miss_count = 0
    # Print results in key: {value1, value2} format
    for callee, caller_tag_pairs in grouped.items():
        values = [[caller,tag] for caller, tag in caller_tag_pairs]
        output = defaultdict(list)
        for value in values:
            output[value[0]].append(value[1])
        for key,value in output.items():
            if "Analysis" in set(value):
                print ("It considers. Modify the logic")
                exit(0)
            if (len(set(value)) != 1):
                # mixed case
                #print (f"Mised case\t{callee}\t{key}")
                func_count[0] += 1
            else:
                # Same case
                func_count[1] += 1
                if value[0] == 'Passed':
                    #print (f"Passed\t{callee}\t{key}")
                    fp_pass.write(f"{callee}\t{key}\n")
                    pass_count += 1
                elif value[0] == 'Missed':
                    #print (f"Missed\t{callee}\t{key}")
                    fp_miss.write(f"{callee}\t{key}\n")
                    miss_count += 1
                else:
                    print (value[0])
    print (func_count, pass_count, miss_count)

def find_opt_files(dirs_or_files):
    all = glob.iglob(os.path.join(dirs_or_files, "**", "*.opt.yaml"), recursive=True)
    return all

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python3 find_same_cases_in_pass.py <yaml_dir> <pass_name>")
        sys.exit(1)
    yaml_dir = sys.argv[1]
    pass_name = sys.argv[2]

    fp_pass = open(pass_name + ".txt", "a")
    fp_miss = open("no_"+ pass_name +".txt", "a")

    all = find_opt_files(yaml_dir)
    func_count = [0, 0]

    for i,filename in enumerate(all):
        print (f"Processing {i} : {filename}")
        main(filename, pass_name, func_count, fp_pass, fp_miss)

    print (func_count)

    fp_pass.close()
    fp_miss.close()