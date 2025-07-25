import subprocess
import sys
import tempfile
import os
import re

def strip_inliner_from_profdata(input_profdata, output_profdata):
    with tempfile.NamedTemporaryFile(delete=False, mode='w+') as temp_txt:
        temp_txt_name = temp_txt.name

    subprocess.run(['llvm-profdata', 'merge', '--text', input_profdata, '-o', temp_txt_name], check=True)

    # Read and filter the text
    with open(temp_txt_name, 'r') as f:
        lines = f.readlines()

    filtered_lines = []
    in_func = False
    keep_func = True
    func_buffer = []

    for line in lines:
        print (line)
        if line.startswith('function: '):
            print (line)
            func_name = line.split('function: ')[1].strip()
            keep_func = not re.search(r'inliner', func_name, re.IGNORECASE)
            in_func = True
            func_buffer = [line]
        elif in_func and line.strip() == '':
            if keep_func:
                filtered_lines.extend(func_buffer)
                filtered_lines.append(line)  # blank line to separate functions
            in_func = False
            print (func_buffer)
            func_buffer = []
        elif in_func:
            func_buffer.append(line)
        else:
            filtered_lines.append(line)

    # Write filtered text to a new temp file
    with tempfile.NamedTemporaryFile(delete=False, mode='w+') as temp_filtered:
        temp_filtered_name = temp_filtered.name
        temp_filtered.writelines(filtered_lines)

    # Re-create a new profdata file
    subprocess.run(['llvm-profdata', 'merge', '--text', temp_filtered_name, '-o', output_profdata], check=True)

    # Clean up
    os.remove(temp_txt_name)
    os.remove(temp_filtered_name)

    print(f"Stripped inliner-related records. Output: {output_profdata}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python strip_inliner_from_profdata.py input.profdata output.profdata")
        sys.exit(1)
    strip_inliner_from_profdata(sys.argv[1], sys.argv[2])
