#!/usr/bin/env python3
"""Render README charts from a GCP validation result JSON file."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap


def load_results(path: Path) -> dict:
    with path.open(encoding="utf-8") as source:
        return json.load(source)


def plot_pass_matrix(results: dict, output: Path) -> None:
    suites = results["suites"]
    names = [suite["name"] for suite in suites]
    matrix = [[int(run["passed"]) for run in suite["runs"]] for suite in suites]

    fig, ax = plt.subplots(figsize=(7.2, 3.5), layout="constrained")
    image = ax.imshow(matrix, cmap=ListedColormap(["#c53b3b", "#2b8a5a"]),
                      vmin=0, vmax=1, aspect="auto")
    del image
    ax.set_xticks(range(3), ["Run 1", "Run 2", "Run 3"])
    ax.set_yticks(range(len(names)), names)
    ax.set_title("GCP end-to-end validation outcome")
    for row, suite in enumerate(suites):
        for column, run in enumerate(suite["runs"]):
            ax.text(column, row, "PASS" if run["passed"] else "FAIL",
                    ha="center", va="center", color="white", weight="bold")
    ax.set_xlabel("Independent harness run")
    ax.set_ylabel("Driver project")
    fig.savefig(output / "pass-matrix.png", dpi=180)
    plt.close(fig)


def plot_workloads(results: dict, output: Path) -> None:
    suites = {suite["id"]: suite for suite in results["suites"]}
    character = suites["character"]["runs"]
    block = suites["block"]["runs"]
    network = suites["network"]["runs"]
    runs = ["Run 1", "Run 2", "Run 3"]
    x = list(range(3))

    fig, (stress_ax, network_ax) = plt.subplots(2, 1, figsize=(8, 6.4), layout="constrained")
    char_rates = [run["stress_mib_per_s"] for run in character]
    block_rates = [run["stress_mib_per_s"] for run in block]
    width = 0.36
    stress_ax.bar([value - width / 2 for value in x], char_rates, width,
                  label="circbuf (validated)", color="#357edd")
    bars = stress_ax.bar([value + width / 2 for value in x], block_rates, width,
                         label="vblk (integrity failed)", color="#c53b3b", hatch="//")
    for bar, rate in zip(bars, block_rates):
        stress_ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                       f"{rate:.2f}\nFAIL", ha="center", va="bottom", fontsize=8)
    stress_ax.set_xticks(x, runs)
    stress_ax.set_ylabel("Observed write rate (MiB/s)")
    stress_ax.set_title("Stress workload measurements")
    stress_ax.legend(loc="upper right")
    stress_ax.grid(axis="y", alpha=0.25)

    bandwidth = [run["udp_sender_mbit_per_s"] for run in network]
    stops = [run["tx_queue_stops"] for run in network]
    network_ax.bar(x, bandwidth, color="#357edd", label="UDP sender bitrate")
    network_ax.set_xticks(x, runs)
    network_ax.set_ylabel("Sender bitrate (Mbit/s)")
    network_ax.set_title("netdrv UDP backpressure workload")
    network_ax.grid(axis="y", alpha=0.25)
    stops_ax = network_ax.twinx()
    stops_ax.plot(x, stops, color="#c97800", marker="o", label="TX queue stops")
    stops_ax.set_ylabel("TX queue stops")
    handles, labels = network_ax.get_legend_handles_labels()
    right_handles, right_labels = stops_ax.get_legend_handles_labels()
    network_ax.legend(handles + right_handles, labels + right_labels, loc="upper left")
    fig.savefig(output / "workload-results.png", dpi=180)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    results = load_results(args.results)
    plot_pass_matrix(results, args.output_dir)
    plot_workloads(results, args.output_dir)


if __name__ == "__main__":
    main()
