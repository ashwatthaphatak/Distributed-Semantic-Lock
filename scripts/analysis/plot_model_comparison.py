# Author: Ashwattha Phatak
#!/usr/bin/env python3
"""Compare DSLM timing plots across embedding models.

Each benchmark_run_*.json file represents one model run. This script groups
runs by model_id and generates side-by-side comparisons using the same timing
metrics as plot_benchmark_report.py.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import textwrap
from pathlib import Path
from statistics import mean
from typing import Any

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
    from matplotlib.patches import Patch
    import numpy as np
except ImportError as exc:
    raise SystemExit(
        "This script requires matplotlib and numpy in the local Python environment."
    ) from exc


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG_DIR = ROOT / "logs"
DEFAULT_PLOTS_ROOT = ROOT / "scripts" / "plots" / "model_comparison"

# --- colour palette -----------------------------------------------------------

GRID = "#cbd5e1"
TEXT = "#0f172a"
WAIT = "#f59e0b"
READ = "#10b981"
WRITE = "#2563eb"
CASE_GREEN = "#059669"
CASE_RED = "#dc2626"
CASE_AMBER = "#d97706"
CASE_PURPLE = "#7c3aed"
FAIL = "#dc2626"

MODEL_PALETTE = [
    "#2563eb",  # blue
    "#dc2626",  # red
    "#059669",  # green
    "#d97706",  # amber
    "#7c3aed",  # purple
    "#0891b2",  # cyan
    "#db2777",  # pink
    "#65a30d",  # lime
]

GROUP_COLORS = {"hotspot": CASE_RED, "mixed": CASE_AMBER, "cold": CASE_GREEN}
GROUP_LABELS = {"hotspot": "Hotspot", "mixed": "Mixed", "cold": "Cold Path"}
GROUP_ORDER = ["hotspot", "mixed", "cold"]
CASE_GROUPS = {
    1: "hotspot",
    2: "mixed",
    3: "hotspot",
    4: "mixed",
    5: "cold",
    6: "hotspot",
    7: "cold",
    8: "hotspot",
    9: "mixed",
    10: "hotspot",
}


# --- helpers ------------------------------------------------------------------


def configure_style() -> None:
    try:
        plt.style.use("seaborn-v0_8-whitegrid")
    except OSError:
        pass
    plt.rcParams.update(
        {
            "figure.facecolor": "white",
            "axes.facecolor": "white",
            "axes.edgecolor": GRID,
            "axes.labelcolor": TEXT,
            "axes.titlecolor": TEXT,
            "xtick.color": TEXT,
            "ytick.color": TEXT,
            "grid.color": GRID,
            "font.size": 10,
            "axes.spines.top": False,
            "axes.spines.right": False,
        }
    )


def slugify(value: str) -> str:
    lowered = value.strip().lower()
    lowered = re.sub(r"[^a-z0-9]+", "_", lowered)
    return lowered.strip("_")


def short_model_name(model_id: str) -> str:
    """Strip registry prefix and :tag suffix for display."""
    name = model_id.split("/")[-1]
    name = re.sub(r":latest$", "", name)
    return name


def model_color(index: int) -> str:
    return MODEL_PALETTE[index % len(MODEL_PALETTE)]


def case_label(case: dict[str, Any], width: int = 16) -> str:
    label = f"C{case['case_index']} {case['name'].removeprefix('The ')}"
    return "\n".join(textwrap.wrap(label, width=width))


def case_group(case: dict[str, Any]) -> str:
    return CASE_GROUPS.get(int(case["case_index"]), "mixed")


def metric(case: dict[str, Any], key: str) -> float:
    return float(case["metrics"].get(key, 0.0))


def average(values: list[float]) -> float:
    return mean(values) if values else 0.0


def blocked_only(values: list[float]) -> list[float]:
    return [v for v in values if v > 0.0]


def successful_ops(case: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        op
        for op in case.get("operations", [])
        if op.get("status_ok") and op.get("granted")
    ]


def op_wait_ms(op: dict[str, Any]) -> float:
    return float(op.get("lock_wait_ms", 0.0))


def op_qdrant_ms(op: dict[str, Any]) -> float:
    start = float(op.get("lock_acquired_unix_ms", 0.0))
    end = float(op.get("qdrant_write_complete_unix_ms", 0.0))
    return max(0.0, end - start)


def op_post_qdrant_ms(op: dict[str, Any]) -> float:
    start = float(op.get("qdrant_write_complete_unix_ms", 0.0))
    end = float(op.get("lock_released_unix_ms", 0.0))
    return max(0.0, end - start)


def op_other_ms(op: dict[str, Any]) -> float:
    elapsed = float(op.get("elapsed_ms", 0.0))
    return max(0.0, elapsed - op_wait_ms(op) - op_qdrant_ms(op) - op_post_qdrant_ms(op))


def case_stats(case: dict[str, Any]) -> dict[str, float]:
    ops = successful_ops(case)
    waits = [op_wait_ms(op) for op in ops]
    blocked = blocked_only(waits)
    qdrant = [op_qdrant_ms(op) for op in ops]
    post = [op_post_qdrant_ms(op) for op in ops]
    other = [op_other_ms(op) for op in ops]
    elapsed = [float(op.get("elapsed_ms", 0.0)) for op in ops]
    scores = [float(op.get("blocking_similarity_score", 0.0)) for op in ops if op.get("blocking_similarity_score")]

    return {
        "avg_wait_all_ms": average(waits),
        "avg_wait_blocked_ms": average(blocked),
        "max_wait_ms": max(waits) if waits else 0.0,
        "blocked_fraction": len(blocked) / len(waits) if waits else 0.0,
        "avg_elapsed_ms": average(elapsed),
        "avg_qdrant_ms": average(qdrant),
        "avg_post_ms": average(post),
        "avg_other_ms": average(other),
        "wait_p95_ms": metric(case, "lock_wait_p95_ms"),
        "latency_p95_ms": metric(case, "latency_p95_ms"),
        "throughput": metric(case, "throughput_ops_per_sec"),
        "serialization_score": metric(case, "serialization_score"),
        "avg_similarity_score": average(scores),
        "max_similarity_score": max(scores) if scores else 0.0,
    }


def run_aggregate_stats(cases: list[dict[str, Any]]) -> dict[str, float]:
    """Aggregate stats across all cases in a run."""
    ops = [op for case in cases for op in successful_ops(case)]
    waits = [op_wait_ms(op) for op in ops]
    blocked = blocked_only(waits)
    qdrant = [op_qdrant_ms(op) for op in ops]
    post = [op_post_qdrant_ms(op) for op in ops]
    other = [op_other_ms(op) for op in ops]
    elapsed = [float(op.get("elapsed_ms", 0.0)) for op in ops]

    return {
        "avg_wait_all_ms": average(waits),
        "avg_wait_blocked_ms": average(blocked),
        "max_wait_ms": max(waits) if waits else 0.0,
        "blocked_fraction": len(blocked) / len(waits) if waits else 0.0,
        "avg_elapsed_ms": average(elapsed),
        "avg_qdrant_ms": average(qdrant),
        "avg_post_ms": average(post),
        "avg_other_ms": average(other),
    }


def group_aggregate_stats(
    cases: list[dict[str, Any]], group_name: str
) -> dict[str, float]:
    group_cases = [c for c in cases if case_group(c) == group_name]
    return run_aggregate_stats(group_cases)


def annotate_bars(
    ax: Any,
    bars: Any,
    fmt: str = "{:.0f}",
    pad: float = 3.0,
    fontsize: int = 7,
) -> None:
    ymax = ax.get_ylim()[1]
    lift = ymax * (pad / 100.0)
    for bar in bars:
        h = bar.get_height()
        if h > 0:
            ax.text(
                bar.get_x() + bar.get_width() / 2.0,
                h + lift,
                fmt.format(h),
                ha="center",
                va="bottom",
                fontsize=fontsize,
                color=TEXT,
            )


def safe_relative(value: float, base: float) -> float:
    return max(0.0, float(value) - base)


# --- data loading -------------------------------------------------------------


def load_run(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def collect_runs(log_dir: Path) -> list[dict[str, Any]]:
    paths = sorted(log_dir.glob("benchmark_run_*.json"), key=lambda p: p.stat().st_mtime)
    if not paths:
        raise SystemExit(f"No benchmark_run_*.json files found under {log_dir}")
    runs = []
    for p in paths:
        try:
            run = load_run(p)
        except Exception as exc:
            print(f"WARNING: skipping {p.name} ({exc})", file=sys.stderr)
            continue
        run["_source_path"] = str(p)
        runs.append(run)
    return runs


def group_by_model(runs: list[dict[str, Any]]) -> dict[str, list[dict[str, Any]]]:
    """Map model_id → list of runs (multiple runs per model are merged/latest kept)."""
    by_model: dict[str, list[dict[str, Any]]] = {}
    for run in runs:
        model = run.get("model_id", "unknown")
        by_model.setdefault(model, []).append(run)
    # Keep the most recent run per model (last in sorted-by-mtime order).
    return {model: runs_list[-1] for model, runs_list in by_model.items()}


def common_case_indices(
    model_runs: dict[str, dict[str, Any]]
) -> list[int]:
    sets = [
        {int(c["case_index"]) for c in run.get("cases", [])}
        for run in model_runs.values()
    ]
    if not sets:
        return []
    common = sets[0].intersection(*sets[1:])
    return sorted(common)


def cases_for_model(run: dict[str, Any], indices: list[int]) -> list[dict[str, Any]]:
    index_set = set(indices)
    return [c for c in run.get("cases", []) if int(c["case_index"]) in index_set]


# --- plot functions -----------------------------------------------------------


def plot_wait_time_summary(
    model_runs: dict[str, dict[str, Any]],
    model_names: list[str],
    case_indices: list[int],
    output_dir: Path,
) -> Path:
    """2x2 grid: avg wait / blocked wait / max wait / blocked fraction, grouped bars per case."""
    cases_by_model = {
        m: {int(c["case_index"]): c for c in model_runs[m].get("cases", [])}
        for m in model_names
    }
    labels = []
    for idx in case_indices:
        # Use name from first model's case
        c = cases_by_model[model_names[0]].get(idx)
        name = c["name"].removeprefix("The ") if c else str(idx)
        label = f"C{idx} " + "\n".join(textwrap.wrap(name, width=12))
        labels.append(label)

    n_cases = len(case_indices)
    n_models = len(model_names)
    total_width = 0.8
    bar_width = total_width / n_models
    x = np.arange(n_cases)

    metric_keys = [
        ("avg_wait_all_ms", "Avg Wait — All Ops", "ms"),
        ("avg_wait_blocked_ms", "Avg Wait — Blocked Ops Only", "ms"),
        ("max_wait_ms", "Worst-Case Wait", "ms"),
        ("blocked_fraction", "Share of Ops That Waited", "%"),
    ]

    fig, axes = plt.subplots(2, 2, figsize=(max(14, n_cases * 1.4), 10), sharex=True)

    for ax, (key, title, unit) in zip(axes.flat, metric_keys):
        for mi, (model_id, color) in enumerate(
            zip(model_names, [model_color(i) for i in range(n_models)])
        ):
            values = []
            for idx in case_indices:
                c = cases_by_model[model_id].get(idx)
                if c is None:
                    values.append(0.0)
                    continue
                v = case_stats(c)[key]
                values.append(v * 100.0 if key == "blocked_fraction" else v)
            offset = (mi - n_models / 2.0 + 0.5) * bar_width
            bars = ax.bar(
                x + offset,
                values,
                width=bar_width * 0.9,
                color=color,
                label=short_model_name(model_id),
                edgecolor="white",
            )
            if n_cases <= 6:
                annotate_bars(ax, bars, fmt="{:.0f}", fontsize=6)

        ax.set_title(title)
        ax.set_ylabel("Milliseconds" if unit == "ms" else "Percent")
        ax.grid(True, axis="y", linestyle="--", linewidth=0.6, alpha=0.7)
        ax.set_xticks(x)
        ax.set_xticklabels(labels, fontsize=8)

    axes[0, 0].legend(frameon=True, loc="upper right", fontsize=8)
    fig.suptitle("Wait Time by Scenario — All Models", fontsize=14, y=1.01)
    fig.tight_layout()
    path = output_dir / "model_01_wait_time_by_case.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_latency_breakdown(
    model_runs: dict[str, dict[str, Any]],
    model_names: list[str],
    case_indices: list[int],
    output_dir: Path,
) -> Path:
    """Stacked latency components per model, aggregated across all cases."""
    n_models = len(model_names)
    x = np.arange(n_models)
    x_labels = [short_model_name(m) for m in model_names]

    agg = [
        run_aggregate_stats(cases_for_model(model_runs[m], case_indices))
        for m in model_names
    ]

    avg_wait = np.array([s["avg_wait_all_ms"] for s in agg])
    avg_qdrant = np.array([s["avg_qdrant_ms"] for s in agg])
    avg_post = np.array([s["avg_post_ms"] for s in agg])
    avg_other = np.array([s["avg_other_ms"] for s in agg])
    avg_elapsed = np.array([s["avg_elapsed_ms"] for s in agg])

    fig, axes = plt.subplots(1, 2, figsize=(14, 6))

    axes[0].bar(x, avg_wait, label="Queue Wait", color=WAIT)
    axes[0].bar(x, avg_qdrant, bottom=avg_wait, label="Qdrant Window", color=CASE_GREEN)
    axes[0].bar(x, avg_post, bottom=avg_wait + avg_qdrant, label="Hold / Release Tail", color=CASE_PURPLE)
    axes[0].bar(x, avg_other, bottom=avg_wait + avg_qdrant + avg_post, label="Other Overhead", color=GRID)
    axes[0].plot(x, avg_elapsed, color=TEXT, marker="o", linewidth=1.8, label="Avg Elapsed")
    axes[0].set_title("Avg Latency Breakdown per Op (All Cases)")
    axes[0].set_ylabel("Milliseconds")
    axes[0].set_xticks(x)
    axes[0].set_xticklabels(x_labels, rotation=15, ha="right")
    axes[0].legend(frameon=True, ncol=3, loc="upper right", fontsize=8)
    axes[0].grid(True, axis="y", linestyle="--", linewidth=0.6, alpha=0.7)

    # Second panel: per group (hotspot / mixed / cold)
    group_width = 0.25
    group_offsets = np.linspace(-group_width, group_width, 3)
    legend_patches = []
    for mi, (model_id, color) in enumerate(
        zip(model_names, [model_color(i) for i in range(n_models)])
    ):
        cases = cases_for_model(model_runs[model_id], case_indices)
        group_agg = [group_aggregate_stats(cases, g) for g in GROUP_ORDER]
        xg = np.arange(len(GROUP_ORDER))
        group_wait = [s["avg_wait_all_ms"] for s in group_agg]
        offset = (mi - n_models / 2.0 + 0.5) * (0.8 / n_models)
        axes[1].bar(
            xg + offset,
            group_wait,
            width=0.8 / n_models * 0.9,
            color=color,
            label=short_model_name(model_id),
            edgecolor="white",
        )
        legend_patches.append(Patch(color=color, label=short_model_name(model_id)))

    axes[1].set_title("Avg Wait by Scenario Group")
    axes[1].set_ylabel("Milliseconds")
    axes[1].set_xticks(np.arange(len(GROUP_ORDER)))
    axes[1].set_xticklabels([GROUP_LABELS[g] for g in GROUP_ORDER])
    axes[1].legend(handles=legend_patches, frameon=True, fontsize=8)
    axes[1].grid(True, axis="y", linestyle="--", linewidth=0.6, alpha=0.7)

    fig.suptitle("Latency Composition — Model Comparison", fontsize=14, y=1.01)
    fig.tight_layout()
    path = output_dir / "model_02_latency_breakdown.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_hotspot_vs_cold(
    model_runs: dict[str, dict[str, Any]],
    model_names: list[str],
    case_indices: list[int],
    output_dir: Path,
) -> Path:
    """Blocked wait cost per group per model — mirrors timing_03."""
    n_models = len(model_names)
    bar_width = 0.8 / n_models
    x = np.arange(len(GROUP_ORDER))

    fig, axes = plt.subplots(1, 2, figsize=(14, 6))

    for mi, (model_id, color) in enumerate(
        zip(model_names, [model_color(i) for i in range(n_models)])
    ):
        cases = cases_for_model(model_runs[model_id], case_indices)
        label = short_model_name(model_id)
        offset = (mi - n_models / 2.0 + 0.5) * bar_width

        all_waits = [
            max(group_aggregate_stats(cases, g)["avg_wait_all_ms"], 0.05)
            for g in GROUP_ORDER
        ]
        blocked_waits = [
            max(group_aggregate_stats(cases, g)["avg_wait_blocked_ms"], 0.05)
            for g in GROUP_ORDER
        ]

        axes[0].bar(x + offset, all_waits, width=bar_width * 0.9, color=color,
                    label=label, alpha=0.85, edgecolor="white")
        axes[1].bar(x + offset, blocked_waits, width=bar_width * 0.9, color=color,
                    label=label, alpha=0.85, edgecolor="white")

    for ax, title in (
        (axes[0], "Avg Wait (All Ops) by Scenario Group"),
        (axes[1], "Avg Wait (Blocked Ops Only) by Scenario Group"),
    ):
        ax.set_yscale("log")
        ax.set_title(title)
        ax.set_ylabel("Milliseconds (log scale)")
        ax.set_xticks(x)
        ax.set_xticklabels([GROUP_LABELS[g] for g in GROUP_ORDER])
        ax.legend(frameon=True, fontsize=8)
        ax.grid(True, axis="y", linestyle="--", linewidth=0.6, alpha=0.7)

    fig.suptitle("Hotspot Tax vs Cold-Path Wait — Model Comparison", fontsize=14, y=1.01)
    fig.tight_layout()
    path = output_dir / "model_03_hotspot_vs_cold.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_conflict_sensitivity(
    model_runs: dict[str, dict[str, Any]],
    model_names: list[str],
    case_indices: list[int],
    output_dir: Path,
) -> Path:
    """Blocked fraction and avg similarity score per case per model.

    Different embedding models may resolve different pairs as semantically
    overlapping, so blocked fraction tells you how 'sensitive' each model is.
    Avg blocking similarity shows how close the embeddings are at the decision
    boundary (high = tighter clusters, low = fuzzier separation).
    """
    n_cases = len(case_indices)
    n_models = len(model_names)
    bar_width = 0.8 / n_models
    x = np.arange(n_cases)

    cases_by_model = {
        m: {int(c["case_index"]): c for c in model_runs[m].get("cases", [])}
        for m in model_names
    }

    labels = []
    for idx in case_indices:
        c = cases_by_model[model_names[0]].get(idx)
        name = c["name"].removeprefix("The ") if c else str(idx)
        labels.append(f"C{idx}\n" + "\n".join(textwrap.wrap(name, width=10)))

    fig, axes = plt.subplots(1, 2, figsize=(max(14, n_cases * 1.4), 6), sharex=True)

    for mi, (model_id, color) in enumerate(
        zip(model_names, [model_color(i) for i in range(n_models)])
    ):
        label = short_model_name(model_id)
        offset = (mi - n_models / 2.0 + 0.5) * bar_width

        blocked_fracs = []
        avg_sims = []
        for idx in case_indices:
            c = cases_by_model[model_id].get(idx)
            if c is None:
                blocked_fracs.append(0.0)
                avg_sims.append(0.0)
                continue
            stats = case_stats(c)
            blocked_fracs.append(stats["blocked_fraction"] * 100.0)
            avg_sims.append(stats["avg_similarity_score"])

        axes[0].bar(x + offset, blocked_fracs, width=bar_width * 0.9,
                    color=color, label=label, edgecolor="white")
        axes[1].bar(x + offset, avg_sims, width=bar_width * 0.9,
                    color=color, label=label, edgecolor="white")

    axes[0].set_title("Blocked Fraction by Case (%)")
    axes[0].set_ylabel("Percent of Ops That Waited")
    axes[0].legend(frameon=True, fontsize=8)

    axes[1].set_title("Avg Blocking Similarity Score by Case")
    axes[1].set_ylabel("Cosine Similarity (at conflict point)")
    axes[1].legend(frameon=True, fontsize=8)

    for ax in axes:
        ax.set_xticks(x)
        ax.set_xticklabels(labels, fontsize=8)
        ax.grid(True, axis="y", linestyle="--", linewidth=0.6, alpha=0.7)

    fig.suptitle("Conflict Sensitivity by Embedding Model", fontsize=14, y=1.01)
    fig.tight_layout()
    path = output_dir / "model_04_conflict_sensitivity.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_case_timeline_comparison(
    case_index: int,
    model_runs: dict[str, dict[str, Any]],
    model_names: list[str],
    output_dir: Path,
) -> Path | None:
    """Per-case Gantt chart with one subplot column per model."""
    cases_by_model: dict[str, dict[str, Any]] = {}
    for m in model_names:
        for c in model_runs[m].get("cases", []):
            if int(c["case_index"]) == case_index:
                cases_by_model[m] = c
                break

    if not cases_by_model:
        return None

    # Determine common agent set across models (by agent_id).
    all_agent_ids: list[str] = []
    seen: set[str] = set()
    for m in model_names:
        case = cases_by_model.get(m)
        if case is None:
            continue
        ops = sorted(case.get("operations", []), key=lambda op: op.get("agent_id", ""))
        for op in ops:
            aid = op["agent_id"]
            if aid not in seen:
                all_agent_ids.append(aid)
                seen.add(aid)

    n_agents = len(all_agent_ids)
    n_models = len(model_names)
    height_per_panel = max(4.5, 0.45 * n_agents + 2.2)
    fig, axes = plt.subplots(
        1, n_models, figsize=(9 * n_models, height_per_panel), sharey=True
    )
    if n_models == 1:
        axes = [axes]

    # Global time base: earliest server_received across all models.
    all_times = []
    for case in cases_by_model.values():
        for op in case.get("operations", []):
            for field in ("server_received_unix_ms", "lock_acquired_unix_ms",
                          "qdrant_write_complete_unix_ms", "lock_released_unix_ms"):
                v = float(op.get(field, 0.0))
                if v > 0.0:
                    all_times.append(v)
    base = min(all_times) if all_times else 0.0

    case_name = ""
    for case in cases_by_model.values():
        case_name = case.get("name", f"Case {case_index}")
        break

    agent_y = {aid: i for i, aid in enumerate(all_agent_ids)}
    y_positions = np.arange(n_agents)

    for ax, model_id in zip(axes, model_names):
        case = cases_by_model.get(model_id)
        if case is None:
            ax.set_title(f"{short_model_name(model_id)}\n(no data)")
            continue

        ops = case.get("operations", [])
        for op in ops:
            aid = op["agent_id"]
            y_pos = agent_y.get(aid, 0)
            start = safe_relative(float(op.get("server_received_unix_ms", 0.0) or 0.0), base)
            acquired = safe_relative(float(op.get("lock_acquired_unix_ms", 0.0) or 0.0), base)
            qdrant = safe_relative(float(op.get("qdrant_write_complete_unix_ms", 0.0) or 0.0), base)
            released = safe_relative(float(op.get("lock_released_unix_ms", 0.0) or 0.0), base)
            status_ok = bool(op.get("status_ok"))
            granted = bool(op.get("granted"))

            if not status_ok or not granted:
                ax.scatter([start], [y_pos], color=FAIL, marker="x", s=80, zorder=3)
                continue

            if acquired > start:
                ax.barh(y_pos, acquired - start, left=start, color=WAIT, height=0.55)
            if released > acquired:
                active_color = READ if op.get("operation") == "read" else WRITE
                ax.barh(y_pos, released - acquired, left=acquired, color=active_color, height=0.55)
            if qdrant > 0.0:
                ax.scatter([qdrant], [y_pos], color=TEXT, marker="o", s=18, zorder=4)

        stats = case_stats(case)
        ax.set_yticks(y_positions)
        ax.set_yticklabels(all_agent_ids)
        ax.invert_yaxis()
        ax.set_xlabel("Time Since First Event (ms)")
        ax.set_title(
            f"{short_model_name(model_id)}\n"
            f"Avg blocked wait {stats['avg_wait_blocked_ms']:.0f} ms | "
            f"Max {stats['max_wait_ms']:.0f} ms"
        )
        ax.grid(True, axis="x", linestyle="--", linewidth=0.6, alpha=0.7)

    legend_items = [
        Patch(facecolor=WAIT, label="Queue Wait"),
        Patch(facecolor=WRITE, label="Write Active"),
        Patch(facecolor=READ, label="Read Active"),
        Line2D([0], [0], color=TEXT, marker="o", linestyle="None", label="Qdrant Complete"),
        Line2D([0], [0], color=FAIL, marker="x", linestyle="None", label="Failed"),
    ]
    axes[0].legend(handles=legend_items, loc="upper right", frameon=True, fontsize=8)

    fig.suptitle(
        f"C{case_index} {case_name} — Timing Timeline by Embedding Model",
        fontsize=13,
        y=1.01,
    )
    fig.tight_layout()
    filename = f"model_timeline_case_{case_index:02d}_{slugify(case_name)}.png"
    path = output_dir / filename
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


# --- orchestration ------------------------------------------------------------


def generate_plots(
    log_dir: Path,
    output_dir: Path,
    skip_timelines: bool,
    model_filter: list[str],
) -> list[Path]:
    runs = collect_runs(log_dir)
    model_runs = group_by_model(runs)

    if model_filter:
        model_runs = {m: r for m, r in model_runs.items() if any(f in m for f in model_filter)}
    if not model_runs:
        raise SystemExit("No runs matched the requested models.")

    if len(model_runs) == 1:
        print(
            f"WARNING: only one model found ({next(iter(model_runs))}). "
            "Comparison plots will be generated but are trivial with a single model."
        )

    model_names = sorted(model_runs.keys())
    case_indices = common_case_indices(model_runs)
    if not case_indices:
        raise SystemExit("No case indices in common across all model runs.")

    output_dir.mkdir(parents=True, exist_ok=True)
    timelines_dir = output_dir / "model_timelines"
    timelines_dir.mkdir(exist_ok=True)

    print(f"Models  : {', '.join(short_model_name(m) for m in model_names)}")
    print(f"Cases   : {case_indices}")
    print(f"Output  : {output_dir}")

    generated = [
        plot_wait_time_summary(model_runs, model_names, case_indices, output_dir),
        plot_latency_breakdown(model_runs, model_names, case_indices, output_dir),
        plot_hotspot_vs_cold(model_runs, model_names, case_indices, output_dir),
        plot_conflict_sensitivity(model_runs, model_names, case_indices, output_dir),
    ]

    if not skip_timelines:
        for idx in case_indices:
            path = plot_case_timeline_comparison(idx, model_runs, model_names, timelines_dir)
            if path is not None:
                generated.append(path)

    return generated


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Generate timing comparison plots across embedding models. "
            "Reads all benchmark_run_*.json files from --log-dir, groups by model_id, "
            "and writes comparison plots to --output-dir."
        )
    )
    parser.add_argument(
        "--log-dir",
        default=str(DEFAULT_LOG_DIR),
        help=f"Directory containing benchmark_run_*.json files. Default: {DEFAULT_LOG_DIR}",
    )
    parser.add_argument(
        "--output-dir",
        default=str(DEFAULT_PLOTS_ROOT),
        help=f"Output directory for generated plots. Default: {DEFAULT_PLOTS_ROOT}",
    )
    parser.add_argument(
        "--skip-timelines",
        action="store_true",
        help="Skip per-case timeline subplots.",
    )
    parser.add_argument(
        "--models",
        nargs="*",
        default=[],
        metavar="MODEL",
        help="Filter to runs whose model_id contains one of these strings.",
    )
    return parser


def main() -> int:
    configure_style()
    args = build_parser().parse_args()
    log_dir = Path(args.log_dir)
    output_dir = Path(args.output_dir)

    generated = generate_plots(log_dir, output_dir, args.skip_timelines, args.models)

    print("Generated files:")
    for p in generated:
        print(f"  - {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
