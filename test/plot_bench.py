#!/usr/bin/env python3
"""
plot_bench.py -- Generate comparison charts from bench.c CSV output.

Usage:
    python3 test/plot_bench.py results.csv
    python3 test/plot_bench.py results.csv --outdir results/
    ./test/bench > results.csv && python3 test/plot_bench.py results.csv
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use('Agg')  # Non-interactive backend
import matplotlib.pyplot as plt
import numpy as np


def load_csv(path):
    """Load benchmark CSV into a list of dicts."""
    rows = []
    with open(path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            row['cardinality'] = int(row['cardinality'])
            row['universe'] = int(row['universe'])
            row['p50_ns'] = float(row['p50_ns'])
            row['p90_ns'] = float(row['p90_ns'])
            row['p99_ns'] = float(row['p99_ns'])
            row['ops_per_sec'] = float(row['ops_per_sec'])
            row['memory_bytes'] = int(row['memory_bytes'])
            rows.append(row)
    return rows


LIBRARY_COLORS = {
    'sparsemap': '#2196F3',   # blue
    'croaring':  '#FF9800',   # orange
    'bitmapset': '#4CAF50',   # green
}

LIBRARY_ORDER = ['sparsemap', 'croaring', 'bitmapset']


def get_color(lib):
    return LIBRARY_COLORS.get(lib, '#999999')


def chart_latency_by_pattern(rows, outdir):
    """Grouped bar chart: for each operation, bars for each library, one chart per pattern."""
    by_pattern = defaultdict(list)
    for r in rows:
        by_pattern[r['pattern']].append(r)

    for pattern, prows in by_pattern.items():
        ops = []
        seen = set()
        for r in prows:
            if r['operation'] not in seen:
                ops.append(r['operation'])
                seen.add(r['operation'])

        libs = [l for l in LIBRARY_ORDER if any(r['library'] == l for r in prows)]
        n_ops = len(ops)
        n_libs = len(libs)

        if n_ops == 0:
            continue

        fig, ax = plt.subplots(figsize=(max(12, n_ops * 1.5), 6))
        x = np.arange(n_ops)
        width = 0.8 / n_libs

        for i, lib in enumerate(libs):
            vals = []
            err_lo = []
            err_hi = []
            for op in ops:
                match = [r for r in prows if r['operation'] == op and r['library'] == lib]
                if match:
                    r = match[0]
                    vals.append(r['p50_ns'])
                    err_lo.append(r['p50_ns'] - r['p50_ns'])  # 0 for lower
                    err_hi.append(r['p99_ns'] - r['p50_ns'])
                else:
                    vals.append(0)
                    err_lo.append(0)
                    err_hi.append(0)

            offset = (i - n_libs / 2 + 0.5) * width
            bars = ax.bar(x + offset, vals, width, label=lib,
                         color=get_color(lib), alpha=0.85,
                         yerr=[err_lo, err_hi], capsize=2)

        ax.set_xlabel('Operation')
        ax.set_ylabel('Latency (ns) - p50 with p99 error bars')
        ax.set_title(f'Operation Latency - Pattern: {pattern}')
        ax.set_xticks(x)
        ax.set_xticklabels(ops, rotation=45, ha='right')
        ax.set_yscale('log')
        ax.legend()
        ax.grid(axis='y', alpha=0.3)
        fig.tight_layout()

        path = os.path.join(outdir, f'latency_{pattern}.png')
        fig.savefig(path, dpi=150)
        plt.close(fig)
        print(f'  Saved {path}')


def chart_memory(rows, outdir):
    """Bar chart comparing memory usage across libraries for each pattern."""
    by_pattern = defaultdict(dict)
    for r in rows:
        if r['operation'] == 'populate':  # Use populate row for memory
            by_pattern[r['pattern']][r['library']] = r['memory_bytes']

    patterns = list(by_pattern.keys())
    libs = [l for l in LIBRARY_ORDER if any(l in by_pattern[p] for p in patterns)]
    n_pat = len(patterns)
    n_libs = len(libs)

    if n_pat == 0:
        return

    fig, ax = plt.subplots(figsize=(max(10, n_pat * 2), 6))
    x = np.arange(n_pat)
    width = 0.8 / n_libs

    for i, lib in enumerate(libs):
        vals = [by_pattern[p].get(lib, 0) for p in patterns]
        offset = (i - n_libs / 2 + 0.5) * width
        ax.bar(x + offset, vals, width, label=lib, color=get_color(lib), alpha=0.85)

    ax.set_xlabel('Pattern')
    ax.set_ylabel('Memory (bytes)')
    ax.set_title('Memory Footprint by Pattern')
    ax.set_xticks(x)
    ax.set_xticklabels(patterns, rotation=45, ha='right')
    ax.set_yscale('log')
    ax.legend()
    ax.grid(axis='y', alpha=0.3)
    fig.tight_layout()

    path = os.path.join(outdir, 'memory_by_pattern.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f'  Saved {path}')


def chart_populate_throughput(rows, outdir):
    """Line plot: populate ops/sec for each library across patterns."""
    pop_rows = [r for r in rows if r['operation'] == 'populate']

    libs = [l for l in LIBRARY_ORDER if any(r['library'] == l for r in pop_rows)]

    fig, ax = plt.subplots(figsize=(10, 6))

    for lib in libs:
        lib_rows = [r for r in pop_rows if r['library'] == lib]
        lib_rows.sort(key=lambda r: r['cardinality'])
        patterns = [r['pattern'] for r in lib_rows]
        throughput = [r['ops_per_sec'] for r in lib_rows]

        ax.bar([f"{p}" for p in patterns], throughput,
               label=lib, color=get_color(lib), alpha=0.7)

    # Use grouped bars instead
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(max(10, len(pop_rows) // 3 * 2), 6))
    by_pattern = defaultdict(dict)
    for r in pop_rows:
        by_pattern[r['pattern']][r['library']] = r['ops_per_sec']

    patterns = list(by_pattern.keys())
    n_pat = len(patterns)
    n_libs = len(libs)
    x = np.arange(n_pat)
    width = 0.8 / n_libs

    for i, lib in enumerate(libs):
        vals = [by_pattern[p].get(lib, 0) for p in patterns]
        offset = (i - n_libs / 2 + 0.5) * width
        ax.bar(x + offset, vals, width, label=lib, color=get_color(lib), alpha=0.85)

    ax.set_xlabel('Pattern')
    ax.set_ylabel('Operations / second')
    ax.set_title('Populate Throughput by Pattern')
    ax.set_xticks(x)
    ax.set_xticklabels(patterns, rotation=45, ha='right')
    ax.set_yscale('log')
    ax.legend()
    ax.grid(axis='y', alpha=0.3)
    fig.tight_layout()

    path = os.path.join(outdir, 'populate_throughput.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f'  Saved {path}')


def chart_percentile_comparison(rows, outdir):
    """For key operations, show p50/p90/p99 as grouped bars."""
    key_ops = ['populate', 'contains', 'iterate', 'offset', 'union']

    for op in key_ops:
        op_rows = [r for r in rows if r['operation'] == op]
        if not op_rows:
            continue

        libs_in_data = [l for l in LIBRARY_ORDER if any(r['library'] == l for r in op_rows)]
        patterns = []
        seen = set()
        for r in op_rows:
            if r['pattern'] not in seen:
                patterns.append(r['pattern'])
                seen.add(r['pattern'])

        if not patterns:
            continue

        fig, axes = plt.subplots(1, len(patterns), figsize=(5 * len(patterns), 5),
                                  sharey=True, squeeze=False)

        for pidx, pat in enumerate(patterns):
            ax = axes[0][pidx]
            pat_rows = [r for r in op_rows if r['pattern'] == pat]
            libs = [l for l in libs_in_data if any(r['library'] == l for r in pat_rows)]

            x = np.arange(len(libs))
            width = 0.25

            p50s = []
            p90s = []
            p99s = []
            for lib in libs:
                match = [r for r in pat_rows if r['library'] == lib]
                if match:
                    p50s.append(match[0]['p50_ns'])
                    p90s.append(match[0]['p90_ns'])
                    p99s.append(match[0]['p99_ns'])
                else:
                    p50s.append(0)
                    p90s.append(0)
                    p99s.append(0)

            ax.bar(x - width, p50s, width, label='p50', color='#2196F3', alpha=0.8)
            ax.bar(x, p90s, width, label='p90', color='#FF9800', alpha=0.8)
            ax.bar(x + width, p99s, width, label='p99', color='#f44336', alpha=0.8)

            ax.set_title(f'{pat}')
            ax.set_xticks(x)
            ax.set_xticklabels([l[:8] for l in libs], rotation=45, ha='right')
            ax.set_yscale('log')
            ax.grid(axis='y', alpha=0.3)
            if pidx == 0:
                ax.set_ylabel('Latency (ns)')
                ax.legend(fontsize=8)

        fig.suptitle(f'Percentile Distribution - {op}', fontsize=14)
        fig.tight_layout()

        path = os.path.join(outdir, f'percentiles_{op}.png')
        fig.savefig(path, dpi=150)
        plt.close(fig)
        print(f'  Saved {path}')


def main():
    parser = argparse.ArgumentParser(description='Plot benchmark results')
    parser.add_argument('csv_file', help='CSV file from bench program')
    parser.add_argument('--outdir', default='results', help='Output directory for PNG files')
    args = parser.parse_args()

    if not os.path.exists(args.csv_file):
        print(f'Error: {args.csv_file} not found', file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.outdir, exist_ok=True)

    print(f'Loading {args.csv_file}...')
    rows = load_csv(args.csv_file)
    print(f'  {len(rows)} data points loaded')

    if not rows:
        print('No data to plot.', file=sys.stderr)
        sys.exit(1)

    print('Generating charts...')
    chart_latency_by_pattern(rows, args.outdir)
    chart_memory(rows, args.outdir)
    chart_populate_throughput(rows, args.outdir)
    chart_percentile_comparison(rows, args.outdir)
    print(f'Done. Charts saved to {args.outdir}/')


if __name__ == '__main__':
    main()
