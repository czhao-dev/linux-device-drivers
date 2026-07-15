#!/usr/bin/env python3
"""Render per-subproject benchmark charts from a project's results JSON.

Each driver subproject keeps its own docs/test-results/<date>/<project>-results.json
(see each subproject's README for how it's produced). This script renders one
PNG per project next to its JSON -- no cross-project combined chart, since each
image is meant to be embedded standalone in that subproject's own README.

Usage:
    python3 scripts/plot_gcp_results.py <project>-results.json
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib.pyplot as plt

# Palette slots taken from the dataviz skill's validated reference palette
# (light mode): categorical slot 1 (blue) for measured values, the fixed
# "good" status color for pass markers, and the standard chart chrome/ink.
BLUE = "#2a78d6"
GOOD = "#0ca30c"
SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRIDLINE = "#e1e0d9"
BASELINE = "#c3c2b7"

plt.rcParams.update({
    "font.family": "sans-serif",
    "text.color": INK_PRIMARY,
    "axes.edgecolor": BASELINE,
    "axes.labelcolor": INK_SECONDARY,
    "xtick.color": INK_MUTED,
    "ytick.color": INK_MUTED,
})


def _run_status_strip(ax, runs: list[dict]) -> None:
    ax.set_xlim(0, len(runs))
    ax.set_ylim(0, 1)
    ax.axis("off")
    for i, run in enumerate(runs):
        passed = run["passed"]
        color = GOOD if passed else "#d03b3b"
        ax.add_patch(plt.Rectangle((i + 0.06, 0.15), 0.88, 0.7, color=color, alpha=0.15))
        ax.text(i + 0.5, 0.5, f"Run {i + 1}\n{'PASS' if passed else 'FAIL'}",
                 ha="center", va="center", color=color, weight="bold", fontsize=10)


def _bar_with_labels(ax, x, values, ylabel, title, fmt="{:.3f}"):
    bars = ax.bar(x, values, width=0.5, color=BLUE)
    for bar, value in zip(bars, values):
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                 fmt.format(value), ha="center", va="bottom",
                 color=INK_SECONDARY, fontsize=9)
    ax.set_xticks(x, [f"Run {i + 1}" for i in x])
    ax.set_ylabel(ylabel)
    ax.set_title(title, color=INK_PRIMARY, fontsize=11, loc="left")
    ax.grid(axis="y", color=GRIDLINE, linewidth=0.8, zorder=0)
    ax.set_axisbelow(True)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)


def plot_stress_project(data: dict, out: Path, label: str) -> None:
    """circbuf / vblk: one throughput metric (MiB/s) across 3 runs."""
    runs = data["runs"]
    x = list(range(len(runs)))
    rates = [r["stress_mib_per_s"] for r in runs]

    fig = plt.figure(figsize=(6.4, 5.2), layout="constrained", facecolor=SURFACE)
    strip_ax, bar_ax = fig.subplots(2, 1, height_ratios=[1, 3])
    strip_ax.set_facecolor(SURFACE)
    bar_ax.set_facecolor(SURFACE)
    _run_status_strip(strip_ax, runs)
    _bar_with_labels(bar_ax, x, rates, "Stress write rate (MiB/s)",
                      f"{label} concurrent stress throughput")
    fig.savefig(out, dpi=180, facecolor=SURFACE)
    plt.close(fig)


def plot_netdrv(data: dict, out: Path) -> None:
    """netdrv: UDP sender bitrate and TX queue stops, as two single-axis charts."""
    runs = data["runs"]
    x = list(range(len(runs)))
    bitrate = [r["udp_sender_mbit_per_s"] for r in runs]
    stops = [r["tx_queue_stops"] for r in runs]

    fig = plt.figure(figsize=(6.4, 7.4), layout="constrained", facecolor=SURFACE)
    strip_ax, bitrate_ax, stops_ax = fig.subplots(3, 1, height_ratios=[1, 3, 3])
    for ax in (strip_ax, bitrate_ax, stops_ax):
        ax.set_facecolor(SURFACE)
    _run_status_strip(strip_ax, runs)
    _bar_with_labels(bitrate_ax, x, bitrate, "Sender bitrate (Mbit/s)",
                      "netdrv UDP backpressure: sender bitrate", fmt="{:.1f}")
    _bar_with_labels(stops_ax, x, stops, "TX queue stops (count)",
                      "netdrv UDP backpressure: TX queue stops (ring_size=4)", fmt="{:.0f}")
    fig.savefig(out, dpi=180, facecolor=SURFACE)
    plt.close(fig)


def plot_pcie(data: dict, out: Path) -> None:
    """pcie: boolean checks only (no magnitude) -- status strip + checklist, not a bar chart."""
    runs = data["runs"]
    checks = runs[0]["checks"]

    fig = plt.figure(figsize=(6.4, 3.6), layout="constrained", facecolor=SURFACE)
    strip_ax, list_ax = fig.subplots(2, 1, height_ratios=[1, 2])
    strip_ax.set_facecolor(SURFACE)
    list_ax.set_facecolor(SURFACE)
    _run_status_strip(strip_ax, runs)

    list_ax.axis("off")
    list_ax.set_title("Guest checks (identical across all 3 runs)",
                       color=INK_PRIMARY, fontsize=11, loc="left")
    for i, check in enumerate(checks):
        y = 1 - (i + 0.5) / len(checks)
        list_ax.text(0.02, y, "✓", color=GOOD, weight="bold", fontsize=12,
                      transform=list_ax.transAxes, va="center")
        list_ax.text(0.09, y, check, color=INK_SECONDARY, fontsize=10,
                      transform=list_ax.transAxes, va="center")
    fig.savefig(out, dpi=180, facecolor=SURFACE)
    plt.close(fig)


PROJECT_LABELS = {"circbuf": "circbuf", "vblk": "vblk"}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    args = parser.parse_args()

    data = json.loads(args.results.read_text())
    project = data["project"]
    out = args.results.with_name(f"{project}-results.png")

    if project in ("circbuf", "vblk"):
        plot_stress_project(data, out, PROJECT_LABELS[project])
    elif project == "netdrv":
        plot_netdrv(data, out)
    elif project == "pcie-edu-driver":
        plot_pcie(data, out)
    else:
        raise SystemExit(f"unknown project: {project}")

    print(f"wrote {out}")


if __name__ == "__main__":
    main()
