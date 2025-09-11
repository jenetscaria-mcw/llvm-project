import yaml
import sys
from deepdiff import DeepDiff

class SafeLoaderIgnoreUnknown(yaml.SafeLoader):
    pass

def unknown_constructor(loader, tag_suffix, node):
    # Preserve the YAML tag (e.g., !Passed / !Missed) on the constructed object
    # so we can compare regressions that are only expressed via the tag.
    tag_full = getattr(node, 'tag', None) or ''
    tag_name = (tag_suffix or tag_full).lstrip('!')
    if isinstance(node, yaml.MappingNode):
        mapping = loader.construct_mapping(node)
        mapping['__yaml_tag__'] = tag_full
        mapping['__yaml_tag_name__'] = tag_name
        return mapping
    elif isinstance(node, yaml.SequenceNode):
        seq = loader.construct_sequence(node)
        return {
            '__yaml_tag__': tag_full,
            '__yaml_tag_name__': tag_name,
            'value': seq,
        }
    else:
        scalar = loader.construct_scalar(node)
        return {
            '__yaml_tag__': tag_full,
            '__yaml_tag_name__': tag_name,
            'value': scalar,
        }

SafeLoaderIgnoreUnknown.add_multi_constructor('', unknown_constructor)

def create_entry_key(entry):
    """Create stable key to match the same optimization opportunity across files.
    Avoid volatile fields like 'Name' that change with decisions, and rely on
    structural identifiers like 'Function' and 'DebugLoc'.
    """
    if isinstance(entry, dict):
        return (
            entry.get('Function', ''),
            str(entry.get('Pass', '')),
            str(entry.get('DebugLoc', {})),
            str(entry.get('Caller', '')),
            str(entry.get('Callee', '')),
            
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
        
        print("=== PGO REGRESSION ANALYSIS ===\n")
        print("Looking for entries where PGO changed 'Passed' decisions to 'Missed'...\n")
        
        # Find matching entries and look for Passed->Missed regressions
        matched_entries = 0
        passed_to_missed_found = 0
        
        for key in master_dict:
            if key in pgo_dict:
                matched_entries += 1
                master_entry = master_dict[key]
                pgo_entry = pgo_dict[key]
                
                # Check for Passed -> Missed regressions
                if check_passed_missed_changes(master_entry, pgo_entry, key):
                    passed_to_missed_found += 1
        
        # Find entries only in master
        only_in_master = set(master_dict.keys()) - set(pgo_dict.keys())
        if only_in_master:
            print("ONLY IN MASTER:")
            for key in only_in_master:
                print(f"  - Function: {key[0]}, Pass: {key[1]}, DebugLoc: {key[2]}")
        
        # Find entries only in PGO
        only_in_pgo = set(pgo_dict.keys()) - set(master_dict.keys())
        if only_in_pgo:
            print("ONLY IN PGO:")
            for key in only_in_pgo:
                print(f"  - Function: {key[0]}, Pass: {key[1]}, DebugLoc: {key[2]}")
        
        print(f"\n=== SUMMARY ===")
        print(f"Total matched entries: {matched_entries}")
        print(f"Passed->Missed regressions found: {passed_to_missed_found}")
        print(f"Master total: {len(master_docs)}, PGO total: {len(pgo_docs)}")
        
        if passed_to_missed_found == 0:
            print("\n⚠️  No Passed->Missed regressions found. This might indicate:")
            print("   - PGO data is not showing decision regressions")
            print("   - The files might be identical")
            print("   - Different field names are used for decision tracking")
        
    except Exception as e:
        print(f"Error: {e}")

def check_passed_missed_changes(master_entry, pgo_entry, key):
    """Check if PGO changed between Passed <-> Missed (both regressions and improvements)."""
    changes_found = []

    # Check each field in both entries
    all_keys = set(master_entry.keys()) | set(pgo_entry.keys())

    for field in all_keys:
        master_value = master_entry.get(field, '')
        pgo_value = pgo_entry.get(field, '')

        # String-based decision changes
        if (isinstance(master_value, str) and isinstance(pgo_value, str)):
            if master_value.lower() == 'passed' and pgo_value.lower() == 'missed':
                changes_found.append({
                    'type': 'REGRESSION',
                    'field': field,
                    'master_value': master_value,
                    'pgo_value': pgo_value
                })
            elif master_value.lower() == 'missed' and pgo_value.lower() == 'passed':
                changes_found.append({
                    'type': 'IMPROVEMENT',
                    'field': field,
                    'master_value': master_value,
                    'pgo_value': pgo_value
                })

    # Explicitly check YAML tag regressions like !Passed -> !Missed and vice versa
    master_tag = master_entry.get('__yaml_tag_name__') if isinstance(master_entry, dict) else None
    pgo_tag = pgo_entry.get('__yaml_tag_name__') if isinstance(pgo_entry, dict) else None
    if isinstance(master_tag, str) and isinstance(pgo_tag, str):
        if master_tag.lower() == 'passed' and pgo_tag.lower() == 'missed':
            changes_found.append({
                'type': 'REGRESSION',
                'field': 'YAML tag',
                'master_value': f"!{master_tag}",
                'pgo_value': f"!{pgo_tag}"
            })
        elif master_tag.lower() == 'missed' and pgo_tag.lower() == 'passed':
            changes_found.append({
                'type': 'IMPROVEMENT',
                'field': 'YAML tag',
                'master_value': f"!{master_tag}",
                'pgo_value': f"!{pgo_tag}"
            })

    # Numeric regressions/improvements (e.g., 1 -> 0, 0 -> 1)
    for field in all_keys:
        master_value = master_entry.get(field)
        pgo_value = pgo_entry.get(field)

        if isinstance(master_value, (int, float, bool)) and isinstance(pgo_value, (int, float, bool)):
            if (master_value == 1 and pgo_value == 0) or (master_value is True and pgo_value is False):
                changes_found.append({
                    'type': 'REGRESSION',
                    'field': field,
                    'master_value': master_value,
                    'pgo_value': pgo_value
                })
            elif (master_value == 0 and pgo_value == 1) or (master_value is False and pgo_value is True):
                changes_found.append({
                    'type': 'IMPROVEMENT',
                    'field': field,
                    'master_value': master_value,
                    'pgo_value': pgo_value
                })

    if changes_found:
        change_types = {c['type'] for c in changes_found}
        marker = "🚨" if "REGRESSION" in change_types else "✅"
        print(f"{marker} PGO CHANGE in Function: {key[0]}")
        print(f"   Pass: {key[1]}, DebugLoc: {key[2]}")
        print(f"   Changes:")
        for change in changes_found:
            print(f"     - {change['type']}: {change['field']} '{change['master_value']}' -> '{change['pgo_value']}'")
        print("   Details (top-level field changes):")
        _print_concise_diff(master_entry, pgo_entry)
        print()
        return True

    return False

def _print_concise_diff(master_entry, pgo_entry):
    """Print a readable summary of top-level field differences between entries."""
    try:
        diff = DeepDiff(master_entry, pgo_entry, ignore_order=True)
    except Exception:
        diff = {}

    # Values changed
    values_changed = diff.get('values_changed', {})
    for path, change in values_changed.items():
        key = _extract_top_level_key(path)
        if key is None:
            continue
        old = change.get('old_value')
        new = change.get('new_value')
        if key in ('__yaml_tag__', '__yaml_tag_name__'):
            continue
        print(f"     * {key}: '{old}' -> '{new}'")

    # Items added/removed at top-level
    added = diff.get('dictionary_item_added', set())
    for path in added:
        key = _extract_top_level_key(path)
        if key is None:
            continue
        if key in ('__yaml_tag__', '__yaml_tag_name__'):
            continue
        print(f"     * Added field: {key} = '{pgo_entry.get(key)}'")

    removed = diff.get('dictionary_item_removed', set())
    for path in removed:
        key = _extract_top_level_key(path)
        if key is None:
            continue
        if key in ('__yaml_tag__', '__yaml_tag_name__'):
            continue
        print(f"     * Removed field: {key} was '{master_entry.get(key)}'")

def _extract_top_level_key(path_str):
    """Extract top-level key from a DeepDiff path like "root['Key']"."""
    try:
        if not isinstance(path_str, str):
            return None
        if not path_str.startswith("root['"):
            return None
        # Only accept simple top-level like root['Key']
        if path_str.endswith("']") and path_str.count("['") == 1:
            return path_str[6:-2]
        return None
    except Exception:
        return None

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python script.py <master_yaml> <pgo_yaml>")
        sys.exit(1)
    
    compare_yaml_by_content(sys.argv[1], sys.argv[2])