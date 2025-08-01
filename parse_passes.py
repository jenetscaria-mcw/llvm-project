import sys
import re

def print_pass_sections(file_path, pass_name):
    # Create output file name based on input file and pass name
    output_file = f"output_{pass_name}.txt"
    
    try:
        with open(file_path, 'r') as f:
            content = f.read()
    except FileNotFoundError:
        print(f"Error: Input file '{file_path}' not found.")
        sys.exit(1)
    except Exception as e:
        print(f"An error occurred while reading the file: {e}")
        sys.exit(1)

    # Split content into sections by '---'
    sections = content.split('---')
    matching_section_count = 0
    filtered_out_count = 0

    # Open output file for writing
    with open(output_file, 'w') as output_f:
        output_f.write("--- Starting Section Search ---\n")
        
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

            # --- Strict Filtering for 'Pass: Inline', '!Passed', 'Added: true', and 'Name: LoadElim' ---
            # is_passed_remark = current_section_lines[0].strip().startswith('!Passed')
            is_passed_remark = current_section_lines[0].strip().startswith(('!Passed', '!Missed'))
            has_added_true = 'Added: true' in full_remark_text #or 'Added: false' in full_remark_text
            has_pass = 'Pass: ' + pass_name in full_remark_text
            # has_load_elim_name = 'Name: LoadElim' in full_remark_text
            
            # --- Check for /usr/lib/gcc/ in DebugLocs within the already filtered section ---
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
            if is_passed_remark and has_added_true and has_pass :
                matching_section_count += 1
                output_f.write(f"\n--- Found Matching Section {matching_section_count} ---\n")
                output_f.write(full_remark_text + "\n") # Write the entire content of the matching section
                output_f.write("---------------------------------------\n") # Separator for clarity

        if matching_section_count == 0:
            output_f.write("\nNo sections found matching all criteria (!Passed, Added: true, Pass: Inline).\n")
        else:
            output_f.write(f"\n--- Finished: Found and printed {matching_section_count} matching sections. ---\n")
    
    print(f"Results saved to: {output_file}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python script_name.py <input.txt> <pass_name>")
        sys.exit(1)

    print_pass_sections(sys.argv[1], sys.argv[2])