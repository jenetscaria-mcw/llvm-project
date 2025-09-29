import json
import numpy as np
import matplotlib.pyplot as plt
from scipy import stats
import pandas as pd
from typing import Dict, List, Tuple
import sys

def load_data(filename: str) -> Dict:
    """
    Load data from JSON file containing Missed and Passed categories
    
    Args:
        filename: Path to the JSON file
        
    Returns:
        Dictionary containing the loaded data
    """
    try:
        with open(filename, 'r') as file:
            data = json.load(file)
        return data
    except FileNotFoundError:
        print(f"Error: File {filename} not found.")
        return {}
    except json.JSONDecodeError:
        print(f"Error: Invalid JSON format in {filename}")
        return {}

def extract_values(data: Dict) -> Tuple[List[float], List[float]]:
    """
    Extract threshold and hotness values from the data structure
    
    Args:
        data: Dictionary with 'Missed' and 'Passed' keys containing lists of [threshold, hotness] pairs
        
    Returns:
        Tuple of (thresholds, hotness_values) lists
    """
    thresholds = []
    hotness_values = []
    
    # Process both Missed and Passed categories
    for category in ['Missed', 'Passed']:
        if category in data:
            category_data = data[category]
            
            # Handle nested structure if it exists (e.g., {'Missed': {'Missed': [[...]]})
            if isinstance(category_data, dict) and category in category_data:
                category_data = category_data[category]
            
            # Extract values from each [threshold, hotness] pair
            for item in category_data:
                if isinstance(item, list) and len(item) >= 2:
                    thresholds.append(float(item[0]))
                    hotness_values.append(float(item[1]))
    
    return thresholds, hotness_values

def calculate_normal_distribution_stats(values: List[float], name: str) -> Dict:
    """
    Calculate normal distribution statistics for a list of values
    
    Args:
        values: List of numerical values
        name: Name for the dataset (for display purposes)
        
    Returns:
        Dictionary containing statistical measures
    """
    if not values:
        print(f"Warning: No values found for {name}")
        return {}
    
    # Convert to numpy array for calculations
    data = np.array(values)
    
    # Calculate basic statistics
    mean = np.mean(data)
    std = np.std(data, ddof=1)  # Sample standard deviation
    variance = np.var(data, ddof=1)
    median = np.median(data)
    
    # Test for normality using Shapiro-Wilk test
    if len(data) >= 3:
        shapiro_stat, shapiro_p = stats.shapiro(data[:5000])  # Shapiro-Wilk works best with smaller samples
    else:
        shapiro_stat, shapiro_p = None, None
    
    # Kolmogorov-Smirnov test for normality
    if len(data) >= 8:
        ks_stat, ks_p = stats.kstest(data, 'norm', args=(mean, std))
    else:
        ks_stat, ks_p = None, None
    
    stats_dict = {
        'name': name,
        'count': len(data),
        'mean': mean,
        'std': std,
        'variance': variance,
        'median': median,
        'min': np.min(data),
        'max': np.max(data),
        'q25': np.percentile(data, 25),
        'q75': np.percentile(data, 75),
        'shapiro_stat': shapiro_stat,
        'shapiro_p': shapiro_p,
        'ks_stat': ks_stat,
        'ks_p': ks_p
    }
    
    return stats_dict

def plot_normal_distribution(values: List[float], name: str, stats_dict: Dict, subplot_pos: int = None):
    """
    Create plots for normal distribution analysis
    
    Args:
        values: List of numerical values
        name: Name for the dataset
        stats_dict: Dictionary containing statistical measures
        subplot_pos: Position for subplot (if creating multiple plots)
    """
    if not values:
        return
    
    data = np.array(values)
    mean = stats_dict['mean']
    std = stats_dict['std']
    
    if subplot_pos:
        plt.subplot(2, 2, subplot_pos)
    
    # Create histogram with normal distribution overlay
    plt.hist(data, bins=30, density=True, alpha=0.7, color='skyblue', edgecolor='black')
    
    # Create normal distribution curve
    x_range = np.linspace(data.min(), data.max(), 100)
    normal_curve = stats.norm.pdf(x_range, mean, std)
    plt.plot(x_range, normal_curve, 'r-', linewidth=2, label=f'Normal(μ={mean:.2f}, σ={std:.2f})')
    
    plt.title(f'{name} Distribution')
    plt.xlabel(name)
    plt.ylabel('Density')
    plt.legend()
    plt.grid(True, alpha=0.3)

def create_qq_plot(values: List[float], name: str, subplot_pos: int = None):
    """
    Create Q-Q plot to assess normality
    
    Args:
        values: List of numerical values
        name: Name for the dataset
        subplot_pos: Position for subplot
    """
    if not values:
        return
    
    if subplot_pos:
        plt.subplot(2, 2, subplot_pos)
    
    stats.probplot(values, dist="norm", plot=plt)
    plt.title(f'Q-Q Plot: {name}')
    plt.grid(True, alpha=0.3)

def print_statistics_report(threshold_stats: Dict, hotness_stats: Dict):
    """
    Print comprehensive statistics report
    
    Args:
        threshold_stats: Statistics dictionary for threshold values
        hotness_stats: Statistics dictionary for hotness values
    """
    print("="*80)
    print("NORMAL DISTRIBUTION ANALYSIS REPORT")
    print("="*80)
    
    for stats_dict in [threshold_stats, hotness_stats]:
        if not stats_dict:
            continue
            
        print(f"\n{stats_dict['name'].upper()} STATISTICS:")
        print("-" * 40)
        print(f"Count:           {stats_dict['count']}")
        print(f"Mean:            {stats_dict['mean']:.4f}")
        print(f"Standard Dev:    {stats_dict['std']:.4f}")
        print(f"Variance:        {stats_dict['variance']:.4f}")
        print(f"Median:          {stats_dict['median']:.4f}")
        print(f"Min:             {stats_dict['min']:.4f}")
        print(f"Max:             {stats_dict['max']:.4f}")
        print(f"25th Percentile: {stats_dict['q25']:.4f}")
        print(f"75th Percentile: {stats_dict['q75']:.4f}")
        
        # Normality tests
        print(f"\nNORMALITY TESTS:")
        if stats_dict['shapiro_p'] is not None:
            print(f"Shapiro-Wilk Test:")
            print(f"  Statistic: {stats_dict['shapiro_stat']:.6f}")
            print(f"  P-value:   {stats_dict['shapiro_p']:.6f}")
            print(f"  Normal?:   {'Yes' if stats_dict['shapiro_p'] > 0.05 else 'No'} (α=0.05)")
        
        if stats_dict['ks_p'] is not None:
            print(f"Kolmogorov-Smirnov Test:")
            print(f"  Statistic: {stats_dict['ks_stat']:.6f}")
            print(f"  P-value:   {stats_dict['ks_p']:.6f}")
            print(f"  Normal?:   {'Yes' if stats_dict['ks_p'] > 0.05 else 'No'} (α=0.05)")

def main():
    """
    Main function to run the normal distribution analysis
    """
    # Configuration
    filename = sys.argv[1]  # Change this to your file path
    
    print("Loading data...")
    data = load_data(filename)
    
    if not data:
        print("Failed to load data. Please check the file path and format.")
        return
    
    print("Extracting threshold and hotness values...")
    thresholds, hotness_values = extract_values(data)
    
    print(f"Found {len(thresholds)} threshold values and {len(hotness_values)} hotness values")
    
    if not thresholds and not hotness_values:
        print("No valid data found. Please check the data format.")
        return
    
    # Calculate statistics
    print("Calculating normal distribution statistics...")
    threshold_stats = calculate_normal_distribution_stats(thresholds, "Threshold") if thresholds else {}
    hotness_stats = calculate_normal_distribution_stats(hotness_values, "Hotness") if hotness_values else {}
    
    # Print statistical report
    print_statistics_report(threshold_stats, hotness_stats)
    
    # Create visualizations
    print("\nGenerating plots...")
    plt.figure(figsize=(15, 10))
    
    # Plot distributions and Q-Q plots
    if thresholds:
        plot_normal_distribution(thresholds, "Threshold", threshold_stats, 1)
        create_qq_plot(thresholds, "Threshold", 2)
    
    if hotness_values:
        plot_normal_distribution(hotness_values, "Hotness", hotness_stats, 3)
        create_qq_plot(hotness_values, "Hotness", 4)
    
    plt.tight_layout()
    plt.savefig('normal_distribution_analysis.png', dpi=300, bbox_inches='tight')
    plt.show()
    
    # Save results to CSV
    results_data = []
    for stats_dict in [threshold_stats, hotness_stats]:
        if stats_dict:
            results_data.append(stats_dict)
    
    if results_data:
        df = pd.DataFrame(results_data)
        df.to_csv('normal_distribution_results.csv', index=False)
        print(f"\nResults saved to 'normal_distribution_results.csv'")
    
    print("\nAnalysis complete!")

if __name__ == "__main__":
    main()