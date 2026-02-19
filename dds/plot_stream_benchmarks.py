#!/usr/bin/env python3
"""
B2: Plot Streaming Benchmark Results (TTFT & ITL)

Reads DDS and HTTP streaming CSVs and generates:
  1. ttft_comparison.png   — TTFT bar chart DDS vs HTTP
  2. itl_comparison.png    — ITL mean / p50 / p95 grouped bars
  3. total_comparison.png  — Total end-to-end time
  4. itl_cdf.png           — CDF of inter-token latencies

Usage: python plot_stream_benchmarks.py <dds_csv> <http_csv> <output_dir>
"""

import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

COLORS = {"DDS": "#2196F3", "HTTP": "#FF9800"}


def load(path, transport):
    df = pd.read_csv(path)
    df["transport"] = transport
    return df


def plot_ttft(dds_df, http_df, outdir):
    fig, ax = plt.subplots(figsize=(8, 5))
    prompt_types = sorted(set(dds_df["prompt_type"].unique()) | set(http_df["prompt_type"].unique()))
    x = np.arange(len(prompt_types))
    w = 0.35

    for i, (label, df) in enumerate([("DDS", dds_df), ("HTTP", http_df)]):
        means, stds = [], []
        for pt in prompt_types:
            sub = df[df["prompt_type"] == pt]
            means.append(sub["ttft_ms"].mean())
            stds.append(sub["ttft_ms"].std())
        offset = -w / 2 + i * w
        # Replace NaN in stds with 0 to avoid StopIteration in matplotlib
        stds = [0.0 if (s != s) else s for s in stds]  # NaN != NaN
        ax.bar(x + offset, means, w, yerr=stds, label=label, color=COLORS[label],
               alpha=0.8, capsize=4)

    ax.set_title("Time-to-First-Token (TTFT)", fontsize=14)
    ax.set_ylabel("TTFT (ms)")
    ax.set_xticks(x)
    ax.set_xticklabels(prompt_types)
    ax.legend()
    ax.grid(True, alpha=0.3, axis="y")
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "ttft_comparison.png"), dpi=150)
    plt.close(fig)
    print("  → ttft_comparison.png")


def plot_itl(dds_df, http_df, outdir):
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    prompt_types = sorted(set(dds_df["prompt_type"].unique()) | set(http_df["prompt_type"].unique()))

    for ax, pt in zip(axes, prompt_types):
        metrics = ["itl_mean_ms", "itl_p50_ms", "itl_p95_ms"]
        metric_labels = ["Mean", "p50", "p95"]
        x = np.arange(len(metrics))
        w = 0.35

        for i, (label, df) in enumerate([("DDS", dds_df), ("HTTP", http_df)]):
            sub = df[df["prompt_type"] == pt]
            vals = [sub[m].mean() for m in metrics]
            offset = -w / 2 + i * w
            ax.bar(x + offset, vals, w, label=label, color=COLORS[label], alpha=0.8)

        ax.set_title(f"Inter-Token Latency — {pt}", fontsize=13)
        ax.set_ylabel("ITL (ms)")
        ax.set_xticks(x)
        ax.set_xticklabels(metric_labels)
        ax.legend()
        ax.grid(True, alpha=0.3, axis="y")

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "itl_comparison.png"), dpi=150)
    plt.close(fig)
    print("  → itl_comparison.png")


def plot_total(dds_df, http_df, outdir):
    fig, ax = plt.subplots(figsize=(8, 5))
    prompt_types = sorted(set(dds_df["prompt_type"].unique()) | set(http_df["prompt_type"].unique()))
    x = np.arange(len(prompt_types))
    w = 0.35

    for i, (label, df) in enumerate([("DDS", dds_df), ("HTTP", http_df)]):
        means = [df[df["prompt_type"] == pt]["total_ms"].mean() for pt in prompt_types]
        stds  = [df[df["prompt_type"] == pt]["total_ms"].std() for pt in prompt_types]
        offset = -w / 2 + i * w
        stds = [0.0 if (s != s) else s for s in stds]
        ax.bar(x + offset, means, w, yerr=stds, label=label, color=COLORS[label],
               alpha=0.8, capsize=4)

    ax.set_title("Total Streaming Time (End-to-End)", fontsize=14)
    ax.set_ylabel("Total (ms)")
    ax.set_xticks(x)
    ax.set_xticklabels(prompt_types)
    ax.legend()
    ax.grid(True, alpha=0.3, axis="y")
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "total_comparison.png"), dpi=150)
    plt.close(fig)
    print("  → total_comparison.png")


def plot_itl_cdf(dds_df, http_df, outdir):
    """CDF of all ITL values by transport for the complex prompt."""
    fig, ax = plt.subplots(figsize=(8, 5))

    for label, df in [("DDS", dds_df), ("HTTP", http_df)]:
        sub = df[df["prompt_type"] == "complex"]
        # ITL values are per-run averages in the CSV; plot them as a CDF
        vals = sorted(sub["itl_mean_ms"].dropna().values)
        if not len(vals):
            continue
        cdf = np.arange(1, len(vals) + 1) / len(vals)
        ax.plot(vals, cdf, label=label, color=COLORS[label], linewidth=2)

    ax.set_title("CDF of ITL (complex prompt)", fontsize=14)
    ax.set_xlabel("ITL mean (ms)")
    ax.set_ylabel("CDF")
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "itl_cdf.png"), dpi=150)
    plt.close(fig)
    print("  → itl_cdf.png")


def main():
    if len(sys.argv) < 3:
        print("Usage: plot_stream_benchmarks.py <dds_csv> <http_csv> [output_dir]")
        sys.exit(1)

    dds_csv  = sys.argv[1]
    http_csv = sys.argv[2]
    outdir   = sys.argv[3] if len(sys.argv) > 3 else "plots_stream"

    os.makedirs(outdir, exist_ok=True)

    dds_df  = load(dds_csv, "DDS")
    http_df = load(http_csv, "HTTP")

    print("Generating streaming benchmark plots...")
    plot_ttft(dds_df, http_df, outdir)
    plot_itl(dds_df, http_df, outdir)
    plot_total(dds_df, http_df, outdir)
    plot_itl_cdf(dds_df, http_df, outdir)
    print("Done.")


if __name__ == "__main__":
    main()
