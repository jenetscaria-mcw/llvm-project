import yaml
import sys
import os
from statistics import mean
from multiprocessing import Pool, cpu_count

# Custom loader to ignore unknown tags (!Passed, !Missed, etc.)
class NoTagLoader(yaml.SafeLoader):
    pass

def unknown_tag_handler(loader, tag_suffix, node):
    data = loader.construct_mapping(node)
    data["_tag"] = tag_suffix.lstrip("!")
    return data

NoTagLoader.add_multi_constructor("!", unknown_tag_handler)

def extract_thresholds_from_file(yaml_file):
    passed, missed = [], []
    try:
        with open(yaml_file, "r") as f:
            docs = yaml.load_all(f, Loader=NoTagLoader)
            for doc in docs:
                if not isinstance(doc, dict):
                    continue
                if doc.get("Pass") != "inline":
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
        print(f"Error reading {yaml_file}: {e}")
    return passed, missed

def collect_thresholds_from_dir(root_dir, workers=None):
    yaml_files = []
    for root, _, files in os.walk(root_dir):
        for fname in files:
            if fname.endswith((".yaml", ".yml")):
                yaml_files.append(os.path.join(root, fname))

    print(f"Found {len(yaml_files)} YAML files under {root_dir}")

    if not yaml_files:
        return [], []

    if workers is None:
        workers = max(1, cpu_count() - 1)  # leave 1 CPU free

    with Pool(processes=workers) as pool:
        results = pool.map(extract_thresholds_from_file, yaml_files)

    all_passed, all_missed = [], []
    for passed, missed in results:
        all_passed.extend(passed)
        all_missed.extend(missed)

    return all_passed, all_missed

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <yaml-directory>")
        sys.exit(1)

    yaml_dir = sys.argv[1]
    passed, missed = collect_thresholds_from_dir(yaml_dir)

    print(f"Total Passed thresholds collected: {len(passed)}")
    print(f"Total Missed thresholds collected: {len(missed)}")

    if passed:
        avg_passed = mean(passed)
        print(f"Overall Average Passed threshold: {avg_passed:.2f}")
    else:
        print("No Passed thresholds found.")
