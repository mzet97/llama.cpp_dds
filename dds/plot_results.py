#!/usr/bin/env python3
"""
Generate comparative plots for DDS vs HTTP benchmarks.
Usage: python3 plot_results.py <dds_csv> <http_csv> <output_dir>
"""

import sys
import os
import pandas as pd
import matplotlib
matplotlib.use("Agg")  # headless / no display required
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np

PROMPT_ORDER = ["simple", "medium", "complex"]
COLORS = {"DDS": "#1f77b4", "HTTP": "#ff7f0e"}


def load(dds_file, http_file):
    dds  = pd.read_csv(dds_file);  dds["protocol"]  = "DDS"
    http = pd.read_csv(http_file); http["protocol"] = "HTTP"
    df = pd.concat([dds, http], ignore_index=True)
    # ensure consistent order
    df["prompt_type"] = pd.Categorical(df["prompt_type"], categories=PROMPT_ORDER, ordered=True)
    df = df.sort_values("prompt_type")
    return df


def bar_group(ax, x, width, dds_vals, http_vals, ylabel, title, fmt="{:.1f}"):
    b1 = ax.bar(x - width/2, dds_vals,  width, label="DDS",  color=COLORS["DDS"],  alpha=0.85)
    b2 = ax.bar(x + width/2, http_vals, width, label="HTTP", color=COLORS["HTTP"], alpha=0.85)
    for bar in b1:
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 2,
                fmt.format(bar.get_height()), ha="center", va="bottom", fontsize=8)
    for bar in b2:
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 2,
                fmt.format(bar.get_height()), ha="center", va="bottom", fontsize=8)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_xticks(x)
    ax.set_xticklabels(PROMPT_ORDER)
    ax.legend()
    ax.grid(axis="y", alpha=0.4)
    ax.yaxis.set_minor_locator(mticker.AutoMinorLocator())


def plot_all(df, output_dir):
    os.makedirs(output_dir, exist_ok=True)

    dds  = df[df["protocol"] == "DDS"].set_index("prompt_type").reindex(PROMPT_ORDER)
    http = df[df["protocol"] == "HTTP"].set_index("prompt_type").reindex(PROMPT_ORDER)
    x    = np.arange(len(PROMPT_ORDER))
    w    = 0.35

    # ── 1. Mean latency ──────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(9, 5))
    bar_group(ax, x, w, dds["mean"], http["mean"],
              "Mean Latency (ms)", "DDS vs HTTP — Mean Latency")
    fig.tight_layout()
    out = os.path.join(output_dir, "latency_mean.png")
    fig.savefig(out, dpi=150);  plt.close(fig)
    print(f"Saved {out}")

    # ── 2. p50 / p95 / p99 side-by-side ─────────────────────────────────────
    fig, axes = plt.subplots(1, 3, figsize=(14, 5), sharey=False)
    for ax, metric in zip(axes, ["p50", "p95", "p99"]):
        bar_group(ax, x, w, dds[metric], http[metric],
                  "Latency (ms)", f"DDS vs HTTP — {metric.upper()}")
    fig.suptitle("Latency Percentiles (DDS vs HTTP)", fontsize=13, fontweight="bold")
    fig.tight_layout()
    out = os.path.join(output_dir, "latency_percentiles.png")
    fig.savefig(out, dpi=150);  plt.close(fig)
    print(f"Saved {out}")

    # ── 3. Speedup factor ────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(8, 5))
    speedup = (http["mean"].values / dds["mean"].values)
    colors  = ["#2ca02c" if s > 1 else "#d62728" for s in speedup]
    bars = ax.bar(PROMPT_ORDER, speedup, color=colors, alpha=0.85)
    ax.axhline(1.0, color="black", linewidth=1.2, linestyle="--", label="Parity (1×)")
    for bar, s in zip(bars, speedup):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.01,
                f"{s:.2f}×", ha="center", va="bottom", fontsize=11, fontweight="bold")
    ax.set_ylabel("Speedup (HTTP mean / DDS mean)")
    ax.set_title("DDS Speedup over HTTP  (>1 = DDS is faster)")
    ax.legend(); ax.grid(axis="y", alpha=0.4)
    fig.tight_layout()
    out = os.path.join(output_dir, "speedup.png")
    fig.savefig(out, dpi=150);  plt.close(fig)
    print(f"Saved {out}")

    # ── 4. Std-dev comparison ────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(9, 5))
    bar_group(ax, x, w, dds["std"], http["std"],
              "Std Dev (ms)", "DDS vs HTTP — Latency Std Deviation (Jitter)")
    fig.tight_layout()
    out = os.path.join(output_dir, "jitter.png")
    fig.savefig(out, dpi=150);  plt.close(fig)
    print(f"Saved {out}")

    # ── 5. Comprehensive summary (2×2) ───────────────────────────────────────
    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    bar_group(axes[0,0], x, w, dds["mean"],  http["mean"],  "ms", "Mean Latency")
    bar_group(axes[0,1], x, w, dds["p50"],   http["p50"],   "ms", "p50 Latency")
    bar_group(axes[1,0], x, w, dds["p95"],   http["p95"],   "ms", "p95 Latency")
    bar_group(axes[1,1], x, w, dds["std"],   http["std"],   "ms", "Std Deviation (Jitter)")

    fig.suptitle("DDS vs HTTP — Comprehensive Benchmark Results\n(TinyLlama 1.1B · 10 req/prompt · max_tokens=30)",
                 fontsize=12, fontweight="bold")
    fig.tight_layout()
    out = os.path.join(output_dir, "summary.png")
    fig.savefig(out, dpi=150);  plt.close(fig)
    print(f"Saved {out}")

    # ── 6. Print text table ──────────────────────────────────────────────────
    print("\n╔══════════════════════════════════════════════════════════════╗")
    print("║            BENCHMARK RESULTS — DDS vs HTTP                  ║")
    print("╠════════╦═══════════╦═══════════╦════════╦═══════════╦═══════╣")
    print("║ Prompt ║ Protocol  ║ Mean (ms) ║ p50    ║ p95       ║ Speedup")
    print("╠════════╬═══════════╬═══════════╬════════╬═══════════╬═══════╣")
    for pt in PROMPT_ORDER:
        d = dds.loc[pt];  h = http.loc[pt]
        sp = h["mean"] / d["mean"]
        print(f"║ {pt:<6} ║ DDS       ║ {d['mean']:>9.1f} ║ {d['p50']:>6.1f} ║ {d['p95']:>9.1f} ║")
        print(f"║ {pt:<6} ║ HTTP      ║ {h['mean']:>9.1f} ║ {h['p50']:>6.1f} ║ {h['p95']:>9.1f} ║  {sp:.2f}×")
        print("╠════════╬═══════════╬═══════════╬════════╬═══════════╬═══════╣")
    print("╚════════╩═══════════╩═══════════╩════════╩═══════════╩═══════╝")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: python3 plot_results.py <dds_csv> <http_csv> <output_dir>")
        sys.exit(1)
    df = load(sys.argv[1], sys.argv[2])
    plot_all(df, sys.argv[3])
    print("Done.")
