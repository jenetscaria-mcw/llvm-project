import yaml
import sys
from deepdiff import DeepDiff

class SafeLoaderIgnoreUnknown(yaml.SafeLoader):
    pass

def unknown_constructor(loader, tag_suffix, node):
    if isinstance(node, yaml.MappingNode):
        return loader.construct_mapping(node)
    elif isinstance(node, yaml.SequenceNode):
        return loader.construct_sequence(node)
    else:
        return loader.construct_scalar(node)

SafeLoaderIgnoreUnknown.add_multi_constructor('', unknown_constructor)

def create_entry_key(entry):
    """Create unique key for matching entries across files"""
    if isinstance(entry, dict):
        return (
            entry.get('Function', ''),
            entry.get('Name', ''),
            entry.get('Pass', ''),
            str(entry.get('DebugLoc', {}))
        )
    return str(entry)

def compare_yaml_by_content(master_file, pgo_file):
    try:
        with open(master_file, 'r') as f1:
            master_docs = list(yaml.load_all(f1, Loader=SafeLoaderIgnoreUnknown))
        
        with open(pgo_file, 'r') as f2:
            pgo_docs = list(yaml.load_all(f2, Loader=SafeLoaderIgnoreUnknown))
        
        
        master_dict = {create_entry_key(doc): doc for doc in master_docs}
        pgo_dict = {create_entry_key(doc): doc for doc in pgo_docs}
        
        print("=== CONTENT-BASED YAML COMPARISON ===\n")
        
        # Find matching entries and compare them
        matched_entries = 0
        for key in master_dict:
            if key in pgo_dict:
                matched_entries += 1
                diff = DeepDiff(master_dict[key], pgo_dict[key], ignore_order=True)
                
                if diff:
                    print(f"DIFFERENCES in entry with Function: {key[0]}")
                    print(f"  Values Changed: {diff.get('values_changed', {})}")
                    print(f"  Items Added: {diff.get('dictionary_item_added', [])}")
                    print(f"  Items Removed: {diff.get('dictionary_item_removed', [])}\n")
                else:
                    print(f"IDENTICAL: {key[0]} matches perfectly")
        
        # Find entries only in master
        only_in_master = set(master_dict.keys()) - set(pgo_dict.keys())
        if only_in_master:
            print("ONLY IN MASTER:")
            for key in only_in_master:
                print(f"  - Function: {key[0]}, Name: {key[1]}, Pass: {key[2]}, DebugLoc: {key[3]}")
        
        # Find entries only in PGO
        only_in_pgo = set(pgo_dict.keys()) - set(master_dict.keys())
        if only_in_pgo:
            print("ONLY IN PGO:")
            for key in only_in_pgo:
                print(f"  - Function: {key[0]}, Name: {key[1]}, Pass: {key[2]}, DebugLoc: {key[3]}")
        
        print(f"\nMatched entries: {matched_entries}")
        print(f"Master total: {len(master_docs)}, PGO total: {len(pgo_docs)}")
        
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python script.py <master_yaml> <pgo_yaml>")
        sys.exit(1)
    
    compare_yaml_by_content(sys.argv[1], sys.argv[2])