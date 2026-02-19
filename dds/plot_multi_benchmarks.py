#!/usr/bin/env python3
"""
B1: Plot Multi-Client Benchmark Results

Reads aggregated CSVs from results/multi/ and generates:
  1. throughput_vs_clients.png  — total throughput (req/s) by client count
  2. p95_vs_clients.png         — tail latency (p95) by client count
  3. mean_latency_vs_clients.png — mean latency by client count
  4. per_client_boxplot.png     — boxplot of per-client latency distribution

Usage: python plot_multi_benchmarks.py <results_dir> <output_dir>
"""

import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

PROMPT_TYPES  = ["simple", "complex"]
CLIENT_COUNTS = [1, 2, 4, 8]
TRANSPORTS    = ["dds", "http"]
COLORS        = {"dds": "#2196F3", "http": "#FF9800"}
LABELS        = {"dds": "DDS (CycloneDDS)", "http": "HTTP (llama-server)"}


def load_agg(results_dir: str) -> pd.DataFrame:
    """Load all aggregated multi-client CSVs into one DataFrame."""
    frames = []
    for n in CLIENT_COUNTS:
        for t in TRANSPORTS:
            path = os.path.join(results_dir, f"{t}_multi_n{n}.csv")
            if not os.path.exists(path):
                continue
            df = pd.read_csv(path)
            df["transport"]    = t
            df["num_clients"]  = n
            frames.append(df)
    if not frames:
        print("No data files found — nothing to plot.")
        sys.exit(1)
    return pd.concat(frames, ignore_index=True)


def plot_throughput(data: pd.DataFrame, outdir: str):
    """Total throughput (req/s) = N_clients * runs / wall_time ≈ estimated from latencies."""
    fig, axes = plt.subplots(1, len(PROMPT_TYPES), figsize=(12, 5), sharey=True)
    if len(PROMPT_TYPES) == 1:
        axes = [axes]

    for ax, ptype in zip(axes, PROMPT_TYPES):
        for t in TRANSPORTS:
            xs, ys = [], []
            for n in CLIENT_COUNTS:
                sub = data[(data["transport"] == t) & (data["num_clients"] == n) & (data["prompt_type"] == ptype)]
                if sub.empty:
                    continue
                # Throughput per client = 1000 / mean_latency_ms (req/s)
                # Total throughput ≈ sum of per-client throughput (since they run in parallel)
                per_client = sub.groupby("client_id")["latency_ms"].mean()
                total_tput = sum(1000.0 / m for m in per_client)
                xs.append(n)
                ys.append(total_tput)
            ax.plot(xs, ys, "o-", color=COLORS[t], label=LABELS[t], linewidth=2, markersize=8)
        ax.set_title(f"Throughput — {ptype}", fontsize=13)
        ax.set_xlabel("Number of Clients")
        ax.set_ylabel("Total Throughput (req/s)")
        ax.set_xticks(CLIENT_COUNTS)
        ax.legend()
        ax.grid(True, alpha=0.3)

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "throughput_vs_clients.png"), dpi=150)
    plt.close(fig)
    print(f"  → throughput_vs_clients.png")


def plot_p95(data: pd.DataFrame, outdir: str):
    """p95 latency vs number of clients."""
    fig, axes = plt.subplots(1, len(PROMPT_TYPES), figsize=(12, 5), sharey=True)
    if len(PROMPT_TYPES) == 1:
        axes = [axes]

    for ax, ptype in zip(axes, PROMPT_TYPES):
        for t in TRANSPORTS:
            xs, ys = [], []
            for n in CLIENT_COUNTS:
                sub = data[(data["transport"] == t) & (data["num_clients"] == n) & (data["prompt_type"] == ptype)]
                if sub.empty:
                    continue
                p95 = np.percentile(sub["latency_ms"], 95)
                xs.append(n)
                ys.append(p95)
            ax.plot(xs, ys, "o-", color=COLORS[t], label=LABELS[t], linewidth=2, markersize=8)
        ax.set_title(f"p95 Latency — {ptype}", fontsize=13)
        ax.set_xlabel("Number of Clients")
        ax.set_ylabel("p95 Latency (ms)")
        ax.set_xticks(CLIENT_COUNTS)
        ax.legend()
        ax.grid(True, alpha=0.3)

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "p95_vs_clients.png"), dpi=150)
    plt.close(fig)
    print(f"  → p95_vs_clients.png")


def plot_mean_latency(data: pd.DataFrame, outdir: str):
    """Mean latency vs number of clients."""
    fig, axes = plt.subplots(1, len(PROMPT_TYPES), figsize=(12, 5), sharey=True)
    if len(PROMPT_TYPES) == 1:
        axes = [axes]

    for ax, ptype in zip(axes, PROMPT_TYPES):
        for t in TRANSPORTS:
            xs, ys = [], []
            for n in CLIENT_COUNTS:
                sub = data[(data["transport"] == t) & (data["num_clients"] == n) & (data["prompt_type"] == ptype)]
                if sub.empty:
                    continue
                xs.append(n)
                ys.append(sub["latency_ms"].mean())
            ax.plot(xs, ys, "o-", color=COLORS[t], label=LABELS[t], linewidth=2, markersize=8)
        ax.set_title(f"Mean Latency — {ptype}", fontsize=13)
        ax.set_xlabel("Number of Clients")
        ax.set_ylabel("Mean Latency (ms)")
        ax.set_xticks(CLIENT_COUNTS)
        ax.legend()
        ax.grid(True, alpha=0.3)

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "mean_latency_vs_clients.png"), dpi=150)
    plt.close(fig)
    print(f"  → mean_latency_vs_clients.png")


def plot_boxplot(data: pd.DataFrame, outdir: str):
    """Per-client latency boxplot for the 8-client scenario."""
    n8 = data[data["num_clients"] == 8]
    if n8.empty:
        n8 = data[data["num_clients"] == data["num_clients"].max()]
    if n8.empty:
        return

    for ptype in PROMPT_TYPES:
        fig, ax = plt.subplots(figsize=(10, 5))
        box_data = []
        box_labels = []
        box_colors = []

        for t in TRANSPORTS:
            sub = n8[(n8["transport"] == t) & (n8["prompt_type"] == ptype)]
            clients = sorted(sub["client_id"].unique())
            for cid in clients:
                csub = sub[sub["client_id"] == cid]
                box_data.append(csub["latency_ms"].values)
                box_labels.append(f"{t.upper()}\nC{cid}")
                box_colors.append(COLORS[t])

        if not box_data:
            plt.close(fig)
            continue

        bp = ax.boxplot(box_data, labels=box_labels, patch_artist=True)
        for patch, color in zip(bp["boxes"], box_colors):
            patch.set_facecolor(color)
            patch.set_alpha(0.6)

        ax.set_title(f"Per-Client Latency Distribution — {ptype} (max clients)", fontsize=13)
        ax.set_ylabel("Latency (ms)")
        ax.grid(True, alpha=0.3, axis="y")
        fig.tight_layout()
        fig.savefig(os.path.join(outdir, f"boxplot_{ptype}.png"), dpi=150)
        plt.close(fig)
        print(f"  → boxplot_{ptype}.png")


def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else "dds/results/multi"
    outdir      = sys.argv[2] if len(sys.argv) > 2 else os.path.join(results_dir, "plots")

    os.makedirs(outdir, exist_ok=True)

    print("Loading multi-client benchmark data...")
    data = load_agg(results_dir)
    print(f"  {len(data)} samples loaded.")

    plot_throughput(data, outdir)
    plot_p95(data, outdir)
    plot_mean_latency(data, outdir)
    plot_boxplot(data, outdir)

    print("Plots done.")


if __name__ == "__main__":
    main()
