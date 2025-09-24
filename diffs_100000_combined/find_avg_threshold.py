import yaml
import sys
import os
from statistics import mean
from multiprocessing import Pool, cpu_count

# --- Custom Loader to ignore unknown tags (!Passed, !Missed, etc.) ---
class NoTagLoader(yaml.SafeLoader):
    pass

def unknown_tag_handler(loader, tag_suffix, node):
    data = loader.construct_mapping(node)
    data["_tag"] = tag_suffix.lstrip("!")
    return data

NoTagLoader.add_multi_constructor("!", unknown_tag_handler)


# --- Function to extract thresholds from one YAML file ---
def extract_thresholds_from_file(yaml_file):
    passed = []
    missed = []
    try:
        with open(yaml_file, "r") as f:
            docs = yaml.load_all(f, Loader=NoTagLoader)
            for doc in docs:
                if not isinstance(doc, dict):
                    continue
                if doc.get("Pass") != "inline":
                    continue
                if not doc.get("Added", False):  # ✅ Only Added: true
                    continue

                args = doc.get("Args", [])
                for arg in args:
                    if isinstance(arg, dict) and "Threshold" in arg:
                        threshold = int(arg["Threshold"])
                        if doc.get("_tag") == "Passed":
                            passed.append(threshold)
                        elif doc.get("_tag") == "Missed":
                            missed.append(threshold)
    except Exception as e:
        print(f"Error processing {yaml_file}: {e}")
    return passed, missed


# --- Wrapper for multiprocessing ---
def process_file(yaml_file):
    return extract_thresholds_from_file(yaml_file)


# --- Collect all YAML files recursively ---
def collect_yaml_files(root_dir):
    yaml_files = []
    for root, _, files in os.walk(root_dir):
        for f in files:
            if f.endswith(".yaml") or f.endswith(".yml"):
                yaml_files.append(os.path.join(root, f))
    return yaml_files


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <yaml-dir>")
        sys.exit(1)

    yaml_dir = sys.argv[1]
    yaml_files = collect_yaml_files(yaml_dir)

    if not yaml_files:
        print(f"No YAML files found in {yaml_dir}")
        sys.exit(1)

    # Use multiprocessing to process files
    with Pool(processes=cpu_count()) as pool:
        results = pool.map(process_file, yaml_files)

    # Collect results
    all_passed = []
    all_missed = []
    for passed, missed in results:
        all_passed.extend(passed)
        all_missed.extend(missed)

    # Print thresholds
    print("Passed thresholds:", all_passed)
    print("Missed thresholds:", all_missed)

    # Print averages
    if all_passed:
        print(f"Average Passed threshold: {mean(all_passed):.2f}")
    else:
        print("No Passed thresholds found.")

    if all_missed:
        print(f"Average Missed threshold: {mean(all_missed):.2f}")
    else:
        print("No Missed thresholds found.")
