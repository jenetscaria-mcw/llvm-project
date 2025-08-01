import sys
import os
import subprocess
import json
import argparse
import re
from typing import Dict, List, Set, Tuple, Optional

class RemarksProfdataCorrelator:
    def __init__(self, llvm_bin_dir: str = ""):
        self.llvm_bin_dir = llvm_bin_dir
        self.llvm_profdata = os.path.join(llvm_bin_dir, "llvm-profdata")
        
    def parse_remarks_file(self, remarks_file: str) -> Dict[str, List[Dict]]:
        remarks_by_pass = {}
        current_section = None
        current_remark = {}
        
        try:
            with open(remarks_file, 'r') as f:
                for line in f:
                    line = line.strip()
                    
                    # New section starts with ---
                    if line.startswith('---'):
                        if current_remark and current_section:
                            if current_section not in remarks_by_pass:
                                remarks_by_pass[current_section] = []
                            remarks_by_pass[current_section].append(current_remark)
                        current_remark = {}
                        current_section = None
                        continue
                    
                    # Extract pass name
                    if line.startswith('Pass:'):
                        current_section = line.split(':', 1)[1].strip()
                        current_remark['pass'] = current_section
                    
                    # Extract function name
                    elif line.startswith('Function:'):
                        current_remark['function'] = line.split(':', 1)[1].strip()
                    
                    # Extract callee information
                    elif line.startswith('- Callee:'):
                        callee = line.split(':', 1)[1].strip()
                        if 'callees' not in current_remark:
                            current_remark['callees'] = []
                        current_remark['callees'].append(callee)
                    
                    # Extract caller information
                    elif line.startswith('- Caller:'):
                        caller = line.split(':', 1)[1].strip()
                        if 'callers' not in current_remark:
                            current_remark['callers'] = []
                        current_remark['callers'].append(caller)
                    
                    # Extract hotness information
                    elif line.startswith('Hotness:'):
                        hotness = line.split(':', 1)[1].strip()
                        current_remark['hotness'] = int(hotness) if hotness.isdigit() else 0
                    
                    # Extract decision information
                    elif line.startswith('Name:'):
                        decision = line.split(':', 1)[1].strip()
                        current_remark['decision'] = decision
                    
                    # Extract cost information
                    elif line.startswith('- Cost:'):
                        cost = line.split(':', 1)[1].strip().strip("'")
                        current_remark['cost'] = int(cost) if cost.replace('-', '').isdigit() else 0
            
            # Don't forget the last remark
            if current_remark and current_section:
                if current_section not in remarks_by_pass:
                    remarks_by_pass[current_section] = []
                remarks_by_pass[current_section].append(current_remark)
                
        except Exception as e:
            print(f"Error parsing remarks file: {e}")
            
        return remarks_by_pass
    
    def analyze_profdata(self, profdata_file: str) -> Dict[str, Dict]:
        try:
            cmd = [self.llvm_profdata, "merge", "--text", profdata_file]

            result = subprocess.run(cmd, capture_output=True, text=True)
            
            if result.returncode != 0:
                print(f"Error running llvm-profdata: {result.stderr}")
                return {}
                
            return self._parse_profdata_output(result.stdout)
            
        except Exception as e:
            print(f"Error analyzing profdata: {e}")
            return {}
    
    def _parse_profdata_output(self, output: str) -> Dict[str, Dict]:
        functions = {}
        current_func = None
        #print (output)
        
        for line in output.split('\n'):
            line = line.strip()
            if not line:
                continue
                
            # Function name
            if not line.startswith('#') and ':' not in line and line:
                current_func = line
                functions[current_func] = {
                    'name': line,
                    'hash': None,
                    'counters': [],
                    'total_count': 0,
                    'hot': False
                }
                
            # Function hash
            elif line.startswith('# Func Hash:'):
                if current_func:
                    hash_val = line.split(':', 1)[1].strip()
                    functions[current_func]['hash'] = hash_val
                    
            # Counter values
            elif current_func and line.isdigit():
                counter_val = int(line)
                functions[current_func]['counters'].append(counter_val)
                functions[current_func]['total_count'] += counter_val
                if counter_val > 0:
                    functions[current_func]['hot'] = True
                    
        return functions
    
    def correlate_remarks_with_profdata(self, remarks_by_pass: Dict, profdata_functions: Dict) -> Dict[str, Set[str]]:
        pass_functions = {}
        
        for pass_name, remarks in remarks_by_pass.items():
            pass_functions[pass_name] = set()
            
            for remark in remarks:
                # Add the main function from the remark
                if 'function' in remark:
                    func_name = remark['function']
                    if func_name in profdata_functions:
                        pass_functions[pass_name].add(func_name)
                
                # Add callee functions
                if 'callees' in remark:
                    for callee in remark['callees']:
                        if callee in profdata_functions:
                            pass_functions[pass_name].add(callee)
                
                # Add caller functions
                if 'callers' in remark:
                    for caller in remark['callers']:
                        if caller in profdata_functions:
                            pass_functions[pass_name].add(caller)
        
        return pass_functions
    
    def create_pass_specific_profdata(self, original_profdata: str, 
                                    pass_functions: Dict[str, Set[str]],
                                    output_dir: str = "pass_variants") -> Dict[str, str]:
        os.makedirs(output_dir, exist_ok=True)
        variant_files = {}
        
        # Get original profdata info
        profdata_functions = self.analyze_profdata(original_profdata)
        
        if not profdata_functions:
            print("Failed to analyze original profdata")
            return {}
        
        for pass_name, functions_to_zero in pass_functions.items():
            print(f"Creating variant for pass: {pass_name}")
            print(f"  Functions to zero: {len(functions_to_zero)}")
            
            variant_file = os.path.join(output_dir, f"no_{pass_name}_prof.profdata")
            
            # Create temporary text file
            temp_file = f"temp_{pass_name}_profdata.txt"
            
            try:
                with open(temp_file, 'w') as f:
                    for func_name, func_info in profdata_functions.items():
                        f.write(f"{func_name}\n")
                        f.write(f"# Func Hash:\n{func_info['hash']}\n")
                        f.write(f"# Num Counters:\n{len(func_info['counters'])}\n")
                        f.write(f"# Counter Values:\n")
                        
                        if func_name in functions_to_zero:
                            # Zero out the function
                            for _ in func_info['counters']:
                                f.write("0\n")
                        else:
                            # Keep original values
                            for counter in func_info['counters']:
                                f.write(f"{counter}\n")
                        f.write("\n")
                
                # Convert to binary profdata
                cmd = [self.llvm_profdata, "merge", "--text", temp_file, "-o", variant_file]
                result = subprocess.run(cmd, capture_output=True, text=True)
                
                if result.returncode == 0:
                    variant_files[pass_name] = variant_file
                    print(f"  Created: {variant_file}")
                else:
                    print(f"  Failed to create variant: {result.stderr}")
                
                # Clean up temp file
                os.remove(temp_file)
                
            except Exception as e:
                print(f"  Error creating variant for {pass_name}: {e}")
        
        return variant_files
    
    def generate_correlation_report(self, remarks_by_pass: Dict, 
                                  pass_functions: Dict[str, Set[str]],
                                  profdata_functions: Dict,
                                  output_file: str = "correlation_report.json") -> Dict:
        report = {
            'summary': {
                'total_passes': len(remarks_by_pass),
                'total_remarks': sum(len(remarks) for remarks in remarks_by_pass.values()),
                'total_profdata_functions': len(profdata_functions),
                'hot_profdata_functions': len([f for f in profdata_functions.values() if f['hot']])
            },
            'pass_analysis': {},
            'correlation_stats': {}
        }
        
        for pass_name, remarks in remarks_by_pass.items():
            pass_functions_set = pass_functions.get(pass_name, set())
            
            # Analyze remarks for this pass
            pass_stats = {
                'total_remarks': len(remarks),
                'functions_in_profdata': len(pass_functions_set),
                'hot_functions': len([f for f in pass_functions_set 
                                    if f in profdata_functions and profdata_functions[f]['hot']]),
                'cold_functions': len([f for f in pass_functions_set 
                                     if f in profdata_functions and not profdata_functions[f]['hot']]),
                'decisions': {}
            }
            
            # Count decisions
            for remark in remarks:
                decision = remark.get('decision', 'Unknown')
                if decision not in pass_stats['decisions']:
                    pass_stats['decisions'][decision] = 0
                pass_stats['decisions'][decision] += 1
            
            report['pass_analysis'][pass_name] = pass_stats
            
            # Correlation stats
            if pass_functions_set:
                correlation = len(pass_functions_set) / len(profdata_functions)
                report['correlation_stats'][pass_name] = {
                    'correlation_ratio': correlation,
                    'functions': list(pass_functions_set)
                }
        
        # Save report
        with open(output_file, 'w') as f:
            json.dump(report, f, indent=2)
        
        print(f"Correlation report saved to: {output_file}")
        return report

def main():
    parser = argparse.ArgumentParser(
        description="Correlate LLVM remarks with profdata for pass-based analysis",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    # Analyze correlation between remarks and profdata
    python remarks_profdata_correlator.py analyze llama_diff.txt app.profdata
    
    # Create pass-specific profdata variants
    python remarks_profdata_correlator.py create-variants llama_diff.txt app.profdata
    
    # Generate detailed correlation report
    python remarks_profdata_correlator.py report llama_diff.txt app.profdata
        """
    )
    
    subparsers = parser.add_subparsers(dest='command', help='Available commands')
    
    # Analyze command
    analyze_parser = subparsers.add_parser('analyze', help='Analyze correlation between remarks and profdata')
    analyze_parser.add_argument('remarks_file', help='LLVM remarks file (e.g., llama_diff.txt)')
    analyze_parser.add_argument('profdata_file', help='Profdata file')
    analyze_parser.add_argument('--output', default='correlation_analysis.json', help='Output file')
    
    # Create variants command
    variants_parser = subparsers.add_parser('create-variants', help='Create pass-specific profdata variants')
    variants_parser.add_argument('remarks_file', help='LLVM remarks file')
    variants_parser.add_argument('profdata_file', help='Profdata file')
    variants_parser.add_argument('--output-dir', default='pass_variants', help='Output directory')
    
    # Report command
    report_parser = subparsers.add_parser('report', help='Generate detailed correlation report')
    report_parser.add_argument('remarks_file', help='LLVM remarks file')
    report_parser.add_argument('profdata_file', help='Profdata file')
    report_parser.add_argument('--output', default='correlation_report.json', help='Output file')
    
    args = parser.parse_args()
    
    if not args.command:
        parser.print_help()
        return
    
    correlator = RemarksProfdataCorrelator()
    
    if args.command == 'analyze':
        print(f"Analyzing correlation between {args.remarks_file} and {args.profdata_file}")
        
        # Parse remarks
        remarks_by_pass = correlator.parse_remarks_file(args.remarks_file)
        print(f"Found {len(remarks_by_pass)} passes in remarks")
        
        # Analyze profdata
        profdata_functions = correlator.analyze_profdata(args.profdata_file)
        print(f"Found {len(profdata_functions)} functions in profdata")
        
        # Correlate
        pass_functions = correlator.correlate_remarks_with_profdata(remarks_by_pass, profdata_functions)
        
        # Save analysis
        analysis = {
            'remarks_by_pass': remarks_by_pass,
            'pass_functions': {pass_name: list(funcs) for pass_name, funcs in pass_functions.items()},
            'profdata_summary': {
                'total_functions': len(profdata_functions),
                'hot_functions': len([f for f in profdata_functions.values() if f['hot']])
            }
        }
        
        with open(args.output, 'w') as f:
            json.dump(analysis, f, indent=2)
        
        print(f"Analysis saved to: {args.output}")
        
        # Print summary
        for pass_name, funcs in pass_functions.items():
            print(f"  {pass_name}: {funcs} functions correlated")
    
    elif args.command == 'create-variants':
        print(f"Creating pass-specific profdata variants")
        
        # Parse remarks
        remarks_by_pass = correlator.parse_remarks_file(args.remarks_file)
        
        # Analyze profdata
        profdata_functions = correlator.analyze_profdata(args.profdata_file)
        
        # Correlate
        pass_functions = correlator.correlate_remarks_with_profdata(remarks_by_pass, profdata_functions)
        
        # Create variants
        variant_files = correlator.create_pass_specific_profdata(
            args.profdata_file, pass_functions, args.output_dir
        )
        
        print(f"\nCreated {len(variant_files)} pass-specific variants:")
        for pass_name, variant_file in variant_files.items():
            print(f"  {pass_name}: {variant_file}")
    
    elif args.command == 'report':
        print(f"Generating detailed correlation report")
        
        # Parse remarks
        remarks_by_pass = correlator.parse_remarks_file(args.remarks_file)
        
        # Analyze profdata
        profdata_functions = correlator.analyze_profdata(args.profdata_file)
        
        # Correlate
        pass_functions = correlator.correlate_remarks_with_profdata(remarks_by_pass, profdata_functions)
        
        # Generate report
        report = correlator.generate_correlation_report(
            remarks_by_pass, pass_functions, profdata_functions, args.output
        )
        
        # Print summary
        print(f"\nCorrelation Summary:")
        print(f"  Total passes: {report['summary']['total_passes']}")
        print(f"  Total remarks: {report['summary']['total_remarks']}")
        print(f"  Total profdata functions: {report['summary']['total_profdata_functions']}")
        print(f"  Hot profdata functions: {report['summary']['hot_profdata_functions']}")
        
        for pass_name, stats in report['correlation_stats'].items():
            print(f"  {pass_name}: {stats['correlation_ratio']:.2%} correlation")

if __name__ == "__main__":
    main() 