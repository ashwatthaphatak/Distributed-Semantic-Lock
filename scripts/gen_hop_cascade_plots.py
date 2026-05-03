"""Generate per-case hop-cascade Gantt charts for all 13 benchmark cases."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).parent.parent

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.colors as mcolors
    import numpy as np
except ImportError:
    sys.exit("This script requires matplotlib and numpy.")

# ── colours ──────────────────────────────────────────────────────────────────
BG = "#1a1a2e"
PANEL = "#16213e"
TEXT = "#e0e0e0"
GREEN = "#4ade80"
RED = "#f87171"
BLUE = "#60a5fa"
AMBER = "#fbbf24"
GRID = "#2a2a4a"

# Hop-depth colour ramp: 0 hops = soft blue, high hops = deep red
HOP_CMAP = plt.cm.get_cmap("YlOrRd")  # yellow → orange → red


def slugify(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def latest_benchmark_json() -> Path:
    candidates = sorted(ROOT.glob("logs/benchmark_run_*.json"))
    if not candidates:
        sys.exit("No benchmark JSON found in logs/")
    return candidates[-1]


def hop_color(hops: int, max_hops: int) -> str:
    if max_hops == 0:
        return BLUE
    t = hops / max_hops
    r, g, b, _ = HOP_CMAP(0.15 + t * 0.85)
    return mcolors.to_hex((r, g, b))


def gen_case_plot(case: dict[str, Any], output_dir: Path) -> Path:
    ops = case["operations"]
    case_idx = int(case["case_index"])
    name = case["name"]

    # Normalise times to ms-since-first-receive
    t0 = min(op["server_received_unix_ms"] for op in ops)

    # Sort agents by time they were first granted (visually cleaner)
    ops_sorted = sorted(ops, key=lambda o: o["lock_acquired_unix_ms"])

    # Short agent labels (strip common prefix)
    agent_ids = [op["agent_id"] for op in ops_sorted]
    prefix = ""
    if len(agent_ids) > 1:
        parts = agent_ids[0].split("_")
        for i in range(1, len(parts)):
            candidate = "_".join(parts[:i]) + "_"
            if all(a.startswith(candidate) for a in agent_ids):
                prefix = candidate
    labels = [a[len(prefix):] if prefix else a for a in agent_ids]

    max_hops = max(op["queue_hops"] for op in ops)
    total_span = max(op["lock_released_unix_ms"] - t0 for op in ops)

    height = max(4.0, len(ops_sorted) * 0.55 + 1.5)
    fig, ax = plt.subplots(figsize=(14, height), facecolor=BG)
    ax.set_facecolor(PANEL)

    bar_h = 0.6

    for y, (op, label) in enumerate(zip(ops_sorted, labels)):
        recv_ms = op["server_received_unix_ms"] - t0
        acq_ms = op["lock_acquired_unix_ms"] - t0
        rel_ms = op["lock_released_unix_ms"] - t0
        hops = op["queue_hops"]
        op_type = op.get("operation", "write")

        wait_dur = acq_ms - recv_ms
        hold_dur = rel_ms - acq_ms

        # Waiting bar
        wc = hop_color(hops, max_hops)
        ax.barh(y, wait_dur, left=recv_ms, height=bar_h,
                color=wc, alpha=0.85, zorder=3)

        # Active lock bar
        lock_color = GREEN if op_type == "write" else BLUE
        ax.barh(y, hold_dur, left=acq_ms, height=bar_h,
                color=lock_color, alpha=0.9, zorder=3)

        # Hop count label inside waiting bar (only if wide enough)
        if wait_dur > total_span * 0.025 and hops > 0:
            ax.text(recv_ms + wait_dur / 2, y, f"{hops}↺",
                    ha="center", va="center", fontsize=8,
                    color="white", fontweight="bold", zorder=4)
        elif hops == 0 and wait_dur > total_span * 0.02:
            ax.text(recv_ms + wait_dur / 2, y, "0",
                    ha="center", va="center", fontsize=7,
                    color="white", alpha=0.7, zorder=4)

    # Y axis
    ax.set_yticks(range(len(ops_sorted)))
    ax.set_yticklabels(labels, fontsize=9, color=TEXT)
    ax.set_ylim(-0.6, len(ops_sorted) - 0.4)
    ax.invert_yaxis()

    # X axis
    ax.set_xlabel("Time since case start (ms)", fontsize=10, color=TEXT)
    ax.tick_params(colors=TEXT, labelsize=9)
    for spine in ax.spines.values():
        spine.set_edgecolor(GRID)
    ax.grid(True, axis="x", linestyle="--", linewidth=0.5, alpha=0.4, color=GRID, zorder=0)

    # Title
    ax.set_title(
        f"Case {case_idx} — {name}\nQueue-Hop Cascade",
        fontsize=13, color=TEXT, fontweight="bold", pad=10,
    )

    # Legend
    legend_elements = [
        plt.Rectangle((0, 0), 1, 1, color=hop_color(0, max(max_hops, 1)), label="Waiting (0 hops)"),
        plt.Rectangle((0, 0), 1, 1, color=hop_color(max(max_hops, 1), max(max_hops, 1)), label=f"Waiting ({max_hops} hops)" if max_hops > 0 else "Waiting"),
        plt.Rectangle((0, 0), 1, 1, color=GREEN, label="Lock held (write)"),
        plt.Rectangle((0, 0), 1, 1, color=BLUE, label="Lock held (read)"),
    ]
    if max_hops == 0:
        legend_elements = legend_elements[2:]
    ax.legend(handles=legend_elements, loc="lower right", fontsize=8,
              facecolor=PANEL, edgecolor=GRID, labelcolor=TEXT, framealpha=0.9)

    # Colorbar for hop depth
    if max_hops > 0:
        sm = plt.cm.ScalarMappable(
            cmap=HOP_CMAP,
            norm=mcolors.Normalize(vmin=0, vmax=max_hops),
        )
        sm.set_array([])
        cbar = fig.colorbar(sm, ax=ax, orientation="vertical", pad=0.01, fraction=0.015)
        cbar.set_label("Queue hops", fontsize=8, color=TEXT)
        cbar.ax.yaxis.set_tick_params(color=TEXT, labelsize=7)
        plt.setp(cbar.ax.yaxis.get_ticklabels(), color=TEXT)

    fig.tight_layout()
    fname = f"hop_cascade_case_{case_idx:02d}_{slugify(name)}.png"
    path = output_dir / fname
    fig.savefig(path, dpi=200, bbox_inches="tight", facecolor=BG)
    plt.close(fig)
    return path


def main() -> None:
    json_path = latest_benchmark_json()
    print(f"Input:  {json_path.relative_to(ROOT)}")

    with open(json_path) as f:
        data = json.load(f)

    run_id = json_path.stem.replace("benchmark_run_", "")
    output_dir = ROOT / "scripts" / "plots" / f"benchmark_run_{run_id}" / "hop_cascades"
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Output: {output_dir.relative_to(ROOT)}/")
    print("Generated files:")
    for case in data["cases"]:
        path = gen_case_plot(case, output_dir)
        print(f"  - {path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
