import sys
import re
import glob
import os

def print_sections(file_path, pass_name, fp_pass, fp_miss):
    try:
        with open(file_path, 'r') as f:
            content = f.read()
    except FileNotFoundError:
        print(f"Error: Input file '{file_path}' not found.")
        sys.exit(1)
    except Exception as e:
        print(f"An error occurred while reading the file: {e}")
        sys.exit(1)

    pass_count = 0
    miss_count = 0

    # Split content into sections by '---'
    sections = content.split('---')
    matching_section_count = 0
    filtered_out_count = 0

    #print("--- Starting Section Search ---")
    
    # Regex to find any DebugLoc block and capture its File path
    debugloc_file_pattern = re.compile(
        r"^\s*DebugLoc:\s*\n" +
        r"(?:^\s*Column:\s*\d+\s*\n)?" + # Column might be optional
        r"^\s*File:\s*(.*?)\s*\n" +      # Capture File path (Group 1)
        r"^\s*Line:\s*\d+\s*\n",         # Line (not captured, just part of the block)
        re.MULTILINE
    )

    for section_content in sections:
        current_section_lines = section_content.strip().splitlines()
        if not current_section_lines: # Skip empty sections
            continue

        full_remark_text = "\n".join(current_section_lines)

        has_added_true = 'Added: true' in full_remark_text
        has_pass = 'Pass: ' + pass_name in full_remark_text
        
        skip_due_to_gcc_path = False
        all_debugloc_matches = debugloc_file_pattern.finditer(full_remark_text)

        for match in all_debugloc_matches:
            file_path_in_debugloc = match.group(1).strip()
            if file_path_in_debugloc.startswith('/usr/lib/gcc/'):
                skip_due_to_gcc_path = True
                filtered_out_count += 1
                # print(f"  Section skipped due to /usr/lib/gcc/ path: {file_path_in_debugloc}") # Debugging GCC path skips
                break # No need to check other DebugLocs in this section

        if skip_due_to_gcc_path:
            continue # Skip this entire section if a GCC path was found

        # Only proceed if all four conditions are met
        if has_added_true and has_pass :
            matching_section_count += 1
            #print(f"\n--- Found Matching Section {matching_section_count} ---")

            callee_match = re.search(r"Callee:\s*(\S+)", full_remark_text)
            caller_match = re.search(r"Caller:\s*(\S+)", full_remark_text)

            if not callee_match or not caller_match:
                return None

            if f"!Passed" not in full_remark_text:
                fp_pass.write(f"{callee_match.group(1)}\t{caller_match.group(1)}\n")
                pass_count += 1

            if f"!Missed" not in full_remark_text:
                fp_miss.write(f"{callee_match.group(1)}\t{caller_match.group(1)}\n")
                miss_count += 1
            
            #print("---------------------------------------") # Separator for clarity

    
    print (f"Pass_count:{pass_count}")
    print (f"Miss_count:{miss_count}")
    # if matching_section_count == 0:
    #    print("\nNo sections found matching all criteria (!Passed, Added: true, Pass: Inline).")
    #else:
    #    print(f"\n--- Finished: Found and printed {matching_section_count} matching sections. ---")

def find_opt_files(dirs_or_files):
    all = glob.iglob(os.path.join(dirs_or_files, "**", "*.opt.yaml"), recursive=True)
    return all

def run_opt_files(dirs_or_files, pass_name):
    all_files = find_opt_files(dirs_or_files)
    print (all_files)
    fp_pass = open("inline.txt", "a")
    fp_miss = open("no_inline.txt", "a")
    
    count = 0
    for file_name in all_files:
        print (f"Processing : {count} : " + file_name)
        count += 1
        print_sections(file_name, pass_name, fp_pass, fp_miss)

    fp_pass.close()
    fp_miss.close()

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python script_name.py <remark_yaml_file> <pass_name>")
        sys.exit(1)

    run_opt_files(sys.argv[1], sys.argv[2])