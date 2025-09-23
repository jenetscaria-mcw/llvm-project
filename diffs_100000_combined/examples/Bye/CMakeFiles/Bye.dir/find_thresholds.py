# import yaml
# import sys

# # Custom loader that ignores unknown tags (!Passed, !Missed, etc.)
# class NoTagLoader(yaml.SafeLoader):
#     pass

# def unknown_tag_handler(loader, tag_suffix, node):
#     # Treat unknown tags as plain mappings
#     return loader.construct_mapping(node)

# NoTagLoader.add_multi_constructor("!", unknown_tag_handler)

# def extract_inline_thresholds(yaml_file):
#     thresholds = []
#     with open(yaml_file, "r") as f:
#         docs = yaml.load_all(f, Loader=NoTagLoader)
#         for doc in docs:
#             if not isinstance(doc, dict):
#                 continue
#             if doc.get("Pass") == "inline":
#                 args = doc.get("Args", [])
#                 for arg in args:
#                     if isinstance(arg, dict) and "Threshold" in arg:
#                         thresholds.append(int(arg["Threshold"]))
#     return thresholds


# if __name__ == "__main__":
#     if len(sys.argv) < 2:
#         print(f"Usage: {sys.argv[0]} <yaml-file>")
#         sys.exit(1)

#     yaml_file = sys.argv[1]
#     values = extract_inline_thresholds(yaml_file)
#     print("Extracted thresholds:", values)

import yaml
import sys
from statistics import mean

# Custom loader that ignores unknown tags (!Passed, !Missed, etc.)
class NoTagLoader(yaml.SafeLoader):
    pass

def unknown_tag_handler(loader, tag_suffix, node):
    # Keep tag type in the mapping (so we know Passed/Missed later)
    data = loader.construct_mapping(node)
    data["_tag"] = tag_suffix.lstrip("!")
    return data

NoTagLoader.add_multi_constructor("!", unknown_tag_handler)

def extract_thresholds(yaml_file):
    passed = []
    missed = []
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
    return passed, missed


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <yaml-file>")
        sys.exit(1)

    yaml_file = sys.argv[1]
    passed, missed = extract_thresholds(yaml_file)

    print("Passed thresholds:", passed)
    print("Missed thresholds:", missed)

    if passed:
        avg_passed = mean(passed)
        print(f"Average Passed threshold: {avg_passed:.2f}")
    else:
        print("No Passed thresholds found.")

