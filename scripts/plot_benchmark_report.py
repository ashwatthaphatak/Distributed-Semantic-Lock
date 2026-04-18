#!/usr/bin/env python3
"""Generate review-ready plots from a curated DSLM benchmark JSON run."""

from __future__ import annotations

import argparse
import json
import re
import sys
import textwrap
from pathlib import Path
from statistics import mean
from typing import Any, Iterable

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
    from matplotlib.patches import Patch
    import numpy as np
except ImportError as exc:  # pragma: no cover - import guard for local use
    raise SystemExit(
        "This script requires matplotlib and numpy in the local Python environment."
    ) from exc


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOG_DIR = ROOT / "logs"
DEFAULT_PLOTS_ROOT = ROOT / "scripts" / "plots"

CASE_BLUE = "#2563eb"
CASE_GREEN = "#059669"
CASE_AMBER = "#d97706"
CASE_RED = "#dc2626"
CASE_PURPLE = "#7c3aed"
GRID = "#cbd5e1"
TEXT = "#0f172a"
WAIT = "#f59e0b"
READ = "#10b981"
WRITE = "#2563eb"
FAIL = "#dc2626"
VIOLATION = "#ef4444"
GROUP_COLORS = {
    "hotspot": CASE_RED,
    "mixed": CASE_AMBER,
    "cold": CASE_GREEN,
}
GROUP_LABELS = {
    "hotspot": "Hotspot",
    "mixed": "Mixed",
    "cold": "Cold Path",
}
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


def repo_relative(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


def find_latest_benchmark(log_dir: Path) -> Path:
    candidates = sorted(log_dir.glob("benchmark_run_*.json"), key=lambda item: item.stat().st_mtime)
    if not candidates:
        raise SystemExit(f"No benchmark_run_*.json files found under {log_dir}")
    return candidates[-1]


def resolve_output_root(raw: str | None) -> Path:
    if not raw:
        return DEFAULT_PLOTS_ROOT
    path = Path(raw)
    return path if path.is_absolute() else ROOT / path


def load_benchmark(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def case_label(case: dict[str, Any], width: int = 18) -> str:
    label = f"C{case['case_index']} {case['name'].removeprefix('The ')}"
    return "\n".join(textwrap.wrap(label, width=width))


def case_code(case: dict[str, Any]) -> str:
    return f"C{case['case_index']}"


def case_group(case: dict[str, Any]) -> str:
    return CASE_GROUPS.get(int(case["case_index"]), "mixed")


def pretty_service_name(raw: str) -> str:
    value = raw
    if value.startswith("dslm-"):
        value = value[len("dslm-") :]
    value = re.sub(r"-\d+$", "", value)
    replacements = {
        "embedding-service": "embedding",
        "dscc-proxy": "proxy",
    }
    return replacements.get(value, value)


def case_color(case: dict[str, Any]) -> str:
    return GROUP_COLORS.get(case_group(case), CASE_BLUE)


def annotate_bars(ax: Any, bars: Iterable[Any], fmt: str = "{:.0f}", pad: float = 3.0) -> None:
    ymax = ax.get_ylim()[1]
    lift = ymax * (pad / 100.0)
    for bar in bars:
        height = bar.get_height()
        ax.text(
            bar.get_x() + bar.get_width() / 2.0,
            height + lift,
            fmt.format(height),
            ha="center",
            va="bottom",
            fontsize=8,
            color=TEXT,
        )


def metric(case: dict[str, Any], key: str) -> float:
    return float(case["metrics"].get(key, 0.0))


def successful_operations(case: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        op
        for op in case.get("operations", [])
        if bool(op.get("status_ok", False)) and bool(op.get("granted", False))
    ]


def average(values: Iterable[float]) -> float:
    collected = list(values)
    return mean(collected) if collected else 0.0


def operation_wait_ms(op: dict[str, Any]) -> float:
    return float(op.get("lock_wait_ms", 0.0))


def operation_qdrant_window_ms(op: dict[str, Any]) -> float:
    start = float(op.get("lock_acquired_unix_ms", 0.0))
    end = float(op.get("qdrant_write_complete_unix_ms", 0.0))
    return max(0.0, end - start)


def operation_post_qdrant_ms(op: dict[str, Any]) -> float:
    start = float(op.get("qdrant_write_complete_unix_ms", 0.0))
    end = float(op.get("lock_released_unix_ms", 0.0))
    return max(0.0, end - start)


def operation_other_ms(op: dict[str, Any]) -> float:
    elapsed = float(op.get("elapsed_ms", 0.0))
    remainder = elapsed - operation_wait_ms(op) - operation_qdrant_window_ms(op) - operation_post_qdrant_ms(op)
    return max(0.0, remainder)


def blocked_only(values: Iterable[float]) -> list[float]:
    return [value for value in values if value > 0.0]


def values_for_type(operations: Iterable[dict[str, Any]], op_type: str, extractor: Any) -> list[float]:
    return [float(extractor(op)) for op in operations if op.get("operation") == op_type]


def case_timing_stats(case: dict[str, Any]) -> dict[str, float]:
    ops = successful_operations(case)
    waits = [operation_wait_ms(op) for op in ops]
    blocked_waits = blocked_only(waits)
    qdrant = [operation_qdrant_window_ms(op) for op in ops]
    post_qdrant = [operation_post_qdrant_ms(op) for op in ops]
    other = [operation_other_ms(op) for op in ops]
    elapsed = [float(op.get("elapsed_ms", 0.0)) for op in ops]

    stats = {
        "avg_wait_all_ms": average(waits),
        "avg_wait_blocked_ms": average(blocked_waits),
        "max_wait_ms": max(waits) if waits else 0.0,
        "blocked_fraction": (len(blocked_waits) / len(waits)) if waits else 0.0,
        "avg_elapsed_ms": average(elapsed),
        "avg_qdrant_ms": average(qdrant),
        "avg_post_qdrant_ms": average(post_qdrant),
        "avg_other_ms": average(other),
        "wait_p95_ms": metric(case, "lock_wait_p95_ms"),
        "latency_p95_ms": metric(case, "latency_p95_ms"),
        "queue_position_p95": metric(case, "queue_position_p95"),
        "wait_position_max": metric(case, "wait_position_max"),
        "queue_hops_max": metric(case, "queue_hops_max"),
    }

    for op_type in ("write", "read"):
        type_waits = values_for_type(ops, op_type, operation_wait_ms)
        blocked_type_waits = blocked_only(type_waits)
        stats[f"{op_type}_avg_wait_all_ms"] = average(type_waits)
        stats[f"{op_type}_avg_wait_blocked_ms"] = average(blocked_type_waits)
        stats[f"{op_type}_max_wait_ms"] = max(type_waits) if type_waits else 0.0
    return stats


def group_timing_stats(cases: list[dict[str, Any]], group_name: str) -> dict[str, float]:
    ops = []
    for case in cases:
        if case_group(case) == group_name:
            ops.extend(successful_operations(case))

    waits = [operation_wait_ms(op) for op in ops]
    blocked_waits = blocked_only(waits)
    elapsed = [float(op.get("elapsed_ms", 0.0)) for op in ops]
    qdrant = [operation_qdrant_window_ms(op) for op in ops]
    post_qdrant = [operation_post_qdrant_ms(op) for op in ops]

    stats = {
        "avg_wait_all_ms": average(waits),
        "avg_wait_blocked_ms": average(blocked_waits),
        "max_wait_ms": max(waits) if waits else 0.0,
        "blocked_fraction": (len(blocked_waits) / len(waits)) if waits else 0.0,
        "avg_elapsed_ms": average(elapsed),
        "avg_qdrant_ms": average(qdrant),
        "avg_post_qdrant_ms": average(post_qdrant),
    }
    for op_type in ("write", "read"):
        type_waits = values_for_type(ops, op_type, operation_wait_ms)
        blocked_type_waits = blocked_only(type_waits)
        stats[f"{op_type}_avg_wait_all_ms"] = average(type_waits)
        stats[f"{op_type}_avg_wait_blocked_ms"] = average(blocked_type_waits)
        stats[f"{op_type}_max_wait_ms"] = max(type_waits) if type_waits else 0.0
    return stats


def timing_legend() -> list[Patch]:
    return [
        Patch(facecolor=GROUP_COLORS[name], label=GROUP_LABELS[name])
        for name in GROUP_ORDER
    ]


def plot_wait_time_summary(cases: list[dict[str, Any]], output_dir: Path) -> Path:
    labels = [case_label(case, width=16) for case in cases]
    x_positions = np.arange(len(cases))
    stats = [case_timing_stats(case) for case in cases]
    colors = [case_color(case) for case in cases]

    fig, axes = plt.subplots(2, 2, figsize=(16, 10), sharex=True)

    avg_all_bars = axes[0, 0].bar(
        x_positions,
        [item["avg_wait_all_ms"] for item in stats],
        color=colors,
        edgecolor="white",
    )
    axes[0, 0].set_title("Average Wait Across All Successful Ops")
    axes[0, 0].set_ylabel("Milliseconds")
    annotate_bars(axes[0, 0], avg_all_bars, fmt="{:.0f}")

    avg_blocked_bars = axes[0, 1].bar(
        x_positions,
        [item["avg_wait_blocked_ms"] for item in stats],
        color=colors,
        edgecolor="white",
    )
    axes[0, 1].set_title("Average Wait Among Blocked Ops Only")
    axes[0, 1].set_ylabel("Milliseconds")
    annotate_bars(axes[0, 1], avg_blocked_bars, fmt="{:.0f}")

    max_wait_bars = axes[1, 0].bar(
        x_positions,
        [item["max_wait_ms"] for item in stats],
        color=colors,
        edgecolor="white",
    )
    axes[1, 0].set_title("Worst-Case Wait")
    axes[1, 0].set_ylabel("Milliseconds")
    annotate_bars(axes[1, 0], max_wait_bars, fmt="{:.0f}")

    blocked_fraction_bars = axes[1, 1].bar(
        x_positions,
        [item["blocked_fraction"] * 100.0 for item in stats],
        color=colors,
        edgecolor="white",
    )
    axes[1, 1].set_title("Share of Successful Ops That Waited")
    axes[1, 1].set_ylabel("Percent")
    annotate_bars(axes[1, 1], blocked_fraction_bars, fmt="{:.0f}%")

    for axis in axes.flat:
        axis.set_xticks(x_positions)
        axis.set_xticklabels(labels)
        axis.grid(True, axis="y", linestyle="--", linewidth=0.6, alpha=0.7)

    axes[0, 0].legend(handles=timing_legend(), frameon=True, loc="upper right")
    fig.suptitle("Timing Summary By Scenario", fontsize=15, y=1.01)
    fig.tight_layout()
    path = output_dir / "timing_01_wait_time_summary.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_latency_component_breakdown(cases: list[dict[str, Any]], output_dir: Path) -> Path:
    labels = [case_label(case, width=16) for case in cases]
    x_positions = np.arange(len(cases))
    stats = [case_timing_stats(case) for case in cases]

    fig, axes = plt.subplots(2, 1, figsize=(16, 10), sharex=True)

    avg_wait = np.array([item["avg_wait_all_ms"] for item in stats])
    avg_qdrant = np.array([item["avg_qdrant_ms"] for item in stats])
    avg_post = np.array([item["avg_post_qdrant_ms"] for item in stats])
    avg_other = np.array([item["avg_other_ms"] for item in stats])
    avg_elapsed = np.array([item["avg_elapsed_ms"] for item in stats])

    axes[0].bar(x_positions, avg_wait, label="Queue Wait", color=WAIT)
    axes[0].bar(
        x_positions,
        avg_qdrant,
        bottom=avg_wait,
        label="Qdrant Window",
        color=CASE_GREEN,
    )
    axes[0].bar(
        x_positions,
        avg_post,
        bottom=avg_wait + avg_qdrant,
        label="Hold / Release Tail",
        color=CASE_PURPLE,
    )
    axes[0].bar(
        x_positions,
        avg_other,
        bottom=avg_wait + avg_qdrant + avg_post,
        label="Other Overhead",
        color=GRID,
    )
    axes[0].plot(
        x_positions,
        avg_elapsed,
        color=TEXT,
        marker="o",
        linewidth=1.8,
        label="Average Elapsed",
    )
    axes[0].set_title("Average Latency Breakdown Per Successful Operation")
    axes[0].set_ylabel("Milliseconds")
    axes[0].legend(frameon=True, ncol=5, loc="upper right")

    width = 0.38
    wait_p95_bars = axes[1].bar(
        x_positions - width / 2.0,
        [item["wait_p95_ms"] for item in stats],
        width=width,
        color=WAIT,
        label="Wait P95",
    )
    latency_p95_bars = axes[1].bar(
        x_positions + width / 2.0,
        [item["latency_p95_ms"] for item in stats],
        width=width,
        color=CASE_BLUE,
        label="Elapsed P95",
    )
    axes[1].set_title("Tail Latency: Wait vs End-To-End")
    axes[1].set_ylabel("Milliseconds")
    axes[1].legend(frameon=True)
    annotate_bars(axes[1], wait_p95_bars, fmt="{:.0f}")
    annotate_bars(axes[1], latency_p95_bars, fmt="{:.0f}")

    for axis in axes:
        axis.set_xticks(x_positions)
        axis.set_xticklabels(labels)
        axis.grid(True, axis="y", linestyle="--", linewidth=0.6, alpha=0.7)

    fig.tight_layout()
    path = output_dir / "timing_02_latency_component_breakdown.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_hotspot_vs_cold_wait(cases: list[dict[str, Any]], output_dir: Path) -> Path:
    summaries = {name: group_timing_stats(cases, name) for name in GROUP_ORDER}
    labels = [GROUP_LABELS[name] for name in GROUP_ORDER]
    x_positions = np.arange(len(GROUP_ORDER))
    width = 0.26

    fig, axes = plt.subplots(1, 2, figsize=(16, 6))

    avg_all_bars = axes[0].bar(
        x_positions - width / 2.0,
        [max(summaries[name]["avg_wait_all_ms"], 0.05) for name in GROUP_ORDER],
        width=width,
        color=CASE_BLUE,
        label="Avg Wait (All Ops)",
    )
    avg_blocked_bars = axes[0].bar(
        x_positions + width / 2.0,
        [max(summaries[name]["avg_wait_blocked_ms"], 0.05) for name in GROUP_ORDER],
        width=width,
        color=CASE_RED,
        label="Avg Wait (Blocked Ops)",
    )
    axes[0].set_yscale("log")
    axes[0].set_title("Hotspot Tax vs Cold-Path Wait")
    axes[0].set_ylabel("Milliseconds (log scale)")
    axes[0].legend(frameon=True)
    for bars, key in (
        (avg_all_bars, "avg_wait_all_ms"),
        (avg_blocked_bars, "avg_wait_blocked_ms"),
    ):
        for bar, group_name in zip(bars, GROUP_ORDER):
            axes[0].text(
                bar.get_x() + bar.get_width() / 2.0,
                bar.get_height() * 1.10,
                f"{summaries[group_name][key]:.2f}",
                ha="center",
                va="bottom",
                fontsize=8,
                color=TEXT,
            )

    write_wait_bars = axes[1].bar(
        x_positions - width / 2.0,
        [max(summaries[name]["write_avg_wait_blocked_ms"], 0.05) for name in GROUP_ORDER],
        width=width,
        color=WRITE,
        label="Write Wait (Blocked Only)",
    )
    read_wait_bars = axes[1].bar(
        x_positions + width / 2.0,
        [max(summaries[name]["read_avg_wait_blocked_ms"], 0.05) for name in GROUP_ORDER],
        width=width,
        color=READ,
        label="Read Wait (Blocked Only)",
    )
    axes[1].set_yscale("log")
    axes[1].set_title("Blocked Wait Cost By Operation Type")
    axes[1].set_ylabel("Milliseconds (log scale)")
    axes[1].legend(frameon=True)
    for bars, key in (
        (write_wait_bars, "write_avg_wait_blocked_ms"),
        (read_wait_bars, "read_avg_wait_blocked_ms"),
    ):
        for bar, group_name in zip(bars, GROUP_ORDER):
            axes[1].text(
                bar.get_x() + bar.get_width() / 2.0,
                bar.get_height() * 1.10,
                f"{summaries[group_name][key]:.2f}",
                ha="center",
                va="bottom",
                fontsize=8,
                color=TEXT,
            )

    for axis in axes:
        axis.set_xticks(x_positions)
        axis.set_xticklabels(labels)
        axis.grid(True, axis="y", linestyle="--", linewidth=0.6, alpha=0.7)

    fig.tight_layout()
    path = output_dir / "timing_03_hotspot_vs_cold_wait.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_queue_depth_vs_wait(cases: list[dict[str, Any]], output_dir: Path) -> Path:
    stats = [case_timing_stats(case) for case in cases]

    fig, axes = plt.subplots(1, 2, figsize=(16, 6))

    for axis, x_key, y_key, title, x_label, y_label in (
        (
            axes[0],
            "wait_position_max",
            "avg_wait_blocked_ms",
            "Queue Depth vs Average Blocked Wait",
            "Max Queue Position",
            "Average Blocked Wait (ms)",
        ),
        (
            axes[1],
            "queue_hops_max",
            "max_wait_ms",
            "Requeue Pressure vs Worst-Case Wait",
            "Max Queue Hops",
            "Worst-Case Wait (ms)",
        ),
    ):
        for case, item in zip(cases, stats):
            axis.scatter(
                max(item[x_key], 0.1),
                max(item[y_key], 0.1),
                s=140.0 + 28.0 * item["blocked_fraction"],
                color=case_color(case),
                edgecolors="#0f172a",
                linewidths=0.8,
                alpha=0.95,
            )
            axis.annotate(
                case_code(case),
                (max(item[x_key], 0.1), max(item[y_key], 0.1)),
                textcoords="offset points",
                xytext=(6, 6),
                fontsize=10,
                fontweight="bold",
                color=TEXT,
            )
        axis.set_xscale("log")
        axis.set_yscale("log")
        axis.set_title(title)
        axis.set_xlabel(x_label)
        axis.set_ylabel(y_label)
        axis.grid(True, which="both", linestyle="--", linewidth=0.6, alpha=0.7)

    axes[0].legend(handles=timing_legend(), frameon=True, loc="lower right", title="Scenario Group")
    fig.tight_layout()
    path = output_dir / "timing_04_queue_depth_vs_wait.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def build_resource_matrix(
    cases: list[dict[str, Any]],
    service_names: list[str],
    metric_name: str,
) -> np.ndarray:
    matrix = np.zeros((len(cases), len(service_names)))
    pretty_names = [pretty_service_name(name) for name in service_names]
    pretty_index = {name: idx for idx, name in enumerate(pretty_names)}
    for row, case in enumerate(cases):
        for stat in case.get("container_stats", []):
            pretty = pretty_service_name(stat["name"])
            column = pretty_index[pretty]
            matrix[row, column] = float(stat.get(metric_name, 0.0))
    return matrix


def plot_resource_heatmaps(cases: list[dict[str, Any]], output_dir: Path) -> Path | None:
    raw_names = sorted({stat["name"] for case in cases for stat in case.get("container_stats", [])})
    if not raw_names:
        return None

    service_names = []
    seen = set()
    for raw_name in raw_names:
        pretty = pretty_service_name(raw_name)
        if pretty in seen:
            continue
        seen.add(pretty)
        service_names.append(pretty)

    matrices = {
        "CPU (%)": build_resource_matrix(cases, raw_names, "cpu_percent"),
        "Memory (MiB)": build_resource_matrix(cases, raw_names, "memory_used_mib"),
        "Net In (MiB)": build_resource_matrix(cases, raw_names, "net_input_mib"),
        "Net Out (MiB)": build_resource_matrix(cases, raw_names, "net_output_mib"),
    }

    fig, axes = plt.subplots(2, 2, figsize=(16, 10))
    y_labels = [case_code(case) for case in cases]
    for axis, (title, values) in zip(axes.flat, matrices.items()):
        image = axis.imshow(values, aspect="auto", cmap="Blues")
        axis.set_title(title)
        axis.set_xticks(np.arange(len(service_names)))
        axis.set_xticklabels(service_names, rotation=35, ha="right")
        axis.set_yticks(np.arange(len(y_labels)))
        axis.set_yticklabels(y_labels)
        fig.colorbar(image, ax=axis, fraction=0.046, pad=0.04)

    fig.suptitle("Per-Case Container Snapshots", fontsize=14, y=1.01)
    fig.tight_layout()
    path = output_dir / "timing_05_resource_heatmaps.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def safe_relative_time(value: float, base: float) -> float:
    return max(0.0, float(value) - base)


def operation_start_time(op: dict[str, Any]) -> float:
    candidates = [
        float(op.get("server_received_unix_ms", 0.0)),
        float(op.get("lock_acquired_unix_ms", 0.0)),
        float(op.get("qdrant_write_complete_unix_ms", 0.0)),
        float(op.get("lock_released_unix_ms", 0.0)),
    ]
    positive = [value for value in candidates if value > 0.0]
    return min(positive) if positive else float(op.get("submit_ms", 0.0))


def plot_case_timeline(case: dict[str, Any], output_dir: Path) -> Path:
    operations = list(case.get("operations", []))
    operations.sort(key=lambda op: (operation_start_time(op), op["agent_id"]))
    stats = case_timing_stats(case)
    times = []
    for op in operations:
        for field in (
            "server_received_unix_ms",
            "lock_acquired_unix_ms",
            "qdrant_write_complete_unix_ms",
            "lock_released_unix_ms",
        ):
            value = float(op.get(field, 0.0))
            if value > 0.0:
                times.append(value)
    base = min(times) if times else 0.0

    height = max(4.5, 0.45 * len(operations) + 2.2)
    fig, ax = plt.subplots(figsize=(14, height))

    y_positions = np.arange(len(operations))
    for y_pos, op in zip(y_positions, operations):
        start = safe_relative_time(float(op.get("server_received_unix_ms", 0.0) or 0.0), base)
        acquired = safe_relative_time(float(op.get("lock_acquired_unix_ms", 0.0) or 0.0), base)
        qdrant = safe_relative_time(float(op.get("qdrant_write_complete_unix_ms", 0.0) or 0.0), base)
        released = safe_relative_time(float(op.get("lock_released_unix_ms", 0.0) or 0.0), base)
        status_ok = bool(op.get("status_ok", False))
        granted = bool(op.get("granted", False))

        if not status_ok or not granted:
            failure_at = start if start > 0.0 else safe_relative_time(float(op.get("submit_ms", 0.0)), 0.0)
            ax.scatter([failure_at], [y_pos], color=FAIL, marker="x", s=80, zorder=3)
            continue

        if acquired > start:
            ax.barh(y_pos, acquired - start, left=start, color=WAIT, height=0.55)
        if released > acquired:
            active_color = READ if op.get("operation") == "read" else WRITE
            ax.barh(y_pos, released - acquired, left=acquired, color=active_color, height=0.55)
        if qdrant > 0.0:
            ax.scatter([qdrant], [y_pos], color=TEXT, marker="o", s=18, zorder=4)

    labels = [f"{op['agent_id']} ({op['operation'][0].upper()})" for op in operations]
    ax.set_yticks(y_positions)
    ax.set_yticklabels(labels)
    ax.invert_yaxis()
    ax.set_xlabel("Time Since First Event (ms)")
    ax.set_title(
        f"{case_code(case)} {case['name']} | "
        f"{GROUP_LABELS[case_group(case)]} | "
        f"Avg blocked wait {stats['avg_wait_blocked_ms']:.0f} ms | "
        f"Max wait {stats['max_wait_ms']:.0f} ms"
    )
    ax.grid(True, axis="x", linestyle="--", linewidth=0.6, alpha=0.7)

    legend_items = [
        Patch(facecolor=WAIT, label="Queue Wait"),
        Patch(facecolor=WRITE, label="Write Active"),
        Patch(facecolor=READ, label="Read Active"),
        Line2D([0], [0], color=TEXT, marker="o", linestyle="None", label="Qdrant Complete"),
        Line2D([0], [0], color=FAIL, marker="x", linestyle="None", label="Failed Operation"),
    ]
    ax.legend(handles=legend_items, loc="upper right", frameon=True)

    fig.tight_layout()
    filename = (
        f"timing_timeline_case_{int(case['case_index']):02d}_{slugify(case['name'])}.png"
    )
    path = output_dir / filename
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def unique_case_sequence(cases: list[dict[str, Any]], key: str, reverse: bool = True) -> list[dict[str, Any]]:
    ordered = sorted(cases, key=lambda case: metric(case, key), reverse=reverse)
    seen = set()
    unique = []
    for case in ordered:
        if case["case_index"] in seen:
            continue
        seen.add(case["case_index"])
        unique.append(case)
    return unique


def write_chart_plan(
    benchmark_path: Path,
    cases: list[dict[str, Any]],
    output_dir: Path,
    generated_files: list[Path],
) -> Path:
    per_case = {int(case["case_index"]): case_timing_stats(case) for case in cases}
    group_stats = {name: group_timing_stats(cases, name) for name in GROUP_ORDER}

    worst_avg_wait = max(cases, key=lambda case: per_case[int(case["case_index"])]["avg_wait_all_ms"])
    worst_blocked_wait = max(cases, key=lambda case: per_case[int(case["case_index"])]["avg_wait_blocked_ms"])
    worst_max_wait = max(cases, key=lambda case: per_case[int(case["case_index"])]["max_wait_ms"])
    deepest_queue = max(cases, key=lambda case: per_case[int(case["case_index"])]["wait_position_max"])
    most_hops = max(cases, key=lambda case: per_case[int(case["case_index"])]["queue_hops_max"])
    cold_baseline = min(
        [case for case in cases if case_group(case) == "cold"],
        key=lambda case: per_case[int(case["case_index"])]["avg_wait_all_ms"],
    )
    strict_cold_case = next(case for case in cases if int(case["case_index"]) == 5)
    ghost_client_case = next(case for case in cases if int(case["case_index"]) == 6)
    read_hotspot_case = next(case for case in cases if int(case["case_index"]) == 3)

    appendix_cases = []
    for case in [worst_max_wait, ghost_client_case, deepest_queue, strict_cold_case, cold_baseline, read_hotspot_case]:
        if case["case_index"] not in {item["case_index"] for item in appendix_cases}:
            appendix_cases.append(case)

    lines = [
        f"# Chart Plan For {benchmark_path.name}",
        "",
        "## Main Deck",
        "1. `timing_01_wait_time_summary.png`",
        "   Open with the queue-cost view: average wait, blocked-only wait, worst-case wait, and blocked fraction by scenario.",
        f"   Current highlights: {worst_avg_wait['name']} has the largest all-op average wait "
        f"({per_case[int(worst_avg_wait['case_index'])]['avg_wait_all_ms']:.0f} ms), "
        f"{worst_blocked_wait['name']} has the largest blocked-only average wait "
        f"({per_case[int(worst_blocked_wait['case_index'])]['avg_wait_blocked_ms']:.0f} ms), and "
        f"{worst_max_wait['name']} has the worst single wait ({per_case[int(worst_max_wait['case_index'])]['max_wait_ms']:.0f} ms).",
        "2. `timing_02_latency_component_breakdown.png`",
        "   Use this to show where latency comes from. In this run, queue wait and hold/release tail dominate; Qdrant itself is comparatively small.",
        f"   Hotspot group average wait is {group_stats['hotspot']['avg_wait_all_ms']:.2f} ms, "
        f"while its average Qdrant window is only {group_stats['hotspot']['avg_qdrant_ms']:.2f} ms.",
        "3. `timing_03_hotspot_vs_cold_wait.png`",
        "   This is the architectural comparison slide: hotspot vs mixed vs cold path.",
        f"   Cold-path traffic still sees tiny waits, but the magnitude stays low: "
        f"avg wait {group_stats['cold']['avg_wait_all_ms']:.2f} ms, "
        f"blocked-only avg {group_stats['cold']['avg_wait_blocked_ms']:.2f} ms, "
        f"max wait {group_stats['cold']['max_wait_ms']:.0f} ms.",
        f"   Hotspot writes are the expensive path: blocked-only write wait averages "
        f"{group_stats['hotspot']['write_avg_wait_blocked_ms']:.2f} ms versus "
        f"{group_stats['cold']['write_avg_wait_blocked_ms']:.2f} ms on the cold path.",
        "4. `timing_04_queue_depth_vs_wait.png`",
        "   Use this to explain that queue depth and queue-hopping amplify timing cost under contention.",
        f"   Queue depth is most visible in {deepest_queue['name']} "
        f"(max wait position {per_case[int(deepest_queue['case_index'])]['wait_position_max']:.0f}) and "
        f"queue hopping is most visible in {most_hops['name']} "
        f"(max queue hops {per_case[int(most_hops['case_index'])]['queue_hops_max']:.0f}).",
        "5. `timing_05_resource_heatmaps.png`",
        "   Keep this as backup only if you want a resource snapshot after the timing story is clear.",
        "",
        "## Timing Story",
        f"- Hotspot group: avg wait {group_stats['hotspot']['avg_wait_all_ms']:.2f} ms, blocked-only avg {group_stats['hotspot']['avg_wait_blocked_ms']:.2f} ms, max wait {group_stats['hotspot']['max_wait_ms']:.0f} ms, blocked fraction {group_stats['hotspot']['blocked_fraction'] * 100.0:.1f}%.",
        f"- Mixed group: avg wait {group_stats['mixed']['avg_wait_all_ms']:.2f} ms, blocked-only avg {group_stats['mixed']['avg_wait_blocked_ms']:.2f} ms, max wait {group_stats['mixed']['max_wait_ms']:.0f} ms, blocked fraction {group_stats['mixed']['blocked_fraction'] * 100.0:.1f}%.",
        f"- Cold group: avg wait {group_stats['cold']['avg_wait_all_ms']:.2f} ms, blocked-only avg {group_stats['cold']['avg_wait_blocked_ms']:.2f} ms, max wait {group_stats['cold']['max_wait_ms']:.0f} ms, blocked fraction {group_stats['cold']['blocked_fraction'] * 100.0:.1f}%.",
        "",
        "## Appendix Timelines",
        "Use the per-case timing timelines to show exactly where time is spent: queue wait, active lock hold, and Qdrant completion.",
        "Recommended appendix cases for this run:",
    ]

    for case in appendix_cases:
        stats = per_case[int(case["case_index"])]
        timeline_name = f"timing_timelines/timing_timeline_case_{int(case['case_index']):02d}_{slugify(case['name'])}.png"
        lines.append(
            f"- `{timeline_name}`: {case['name']} | "
            f"avg blocked wait {stats['avg_wait_blocked_ms']:.0f} ms | "
            f"max wait {stats['max_wait_ms']:.0f} ms | "
            f"wait P95 {stats['wait_p95_ms']:.0f} ms"
        )

    lines.extend(
        [
            "",
            "## Notes",
            "- The curated benchmark latency numbers exclude embedding generation because embeddings are precomputed before the per-case run begins.",
            "- The main architectural cost here is queue wait plus any hold/release tail, not Qdrant execution time.",
            "- Container charts are one-shot snapshots collected after each case, not continuous peak measurements.",
            "- All generated outputs for this run are listed below.",
            "",
        ]
    )
    for path in generated_files:
        lines.append(f"- `{path.name if path.parent == output_dir else path.relative_to(output_dir)}`")

    plan_path = output_dir / "timing_chart_plan.md"
    plan_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return plan_path


def generate_plots(
    benchmark_path: Path,
    output_root: Path,
    skip_timelines: bool,
) -> list[Path]:
    data = load_benchmark(benchmark_path)
    cases = list(data.get("cases", []))
    if not cases:
        raise SystemExit(f"No cases found in {benchmark_path}")

    run_output_dir = output_root / benchmark_path.stem
    timings_dir = run_output_dir
    timelines_dir = run_output_dir / "timing_timelines"
    timelines_dir.mkdir(parents=True, exist_ok=True)

    generated = [
        plot_wait_time_summary(cases, timings_dir),
        plot_latency_component_breakdown(cases, timings_dir),
        plot_hotspot_vs_cold_wait(cases, timings_dir),
        plot_queue_depth_vs_wait(cases, timings_dir),
    ]

    resources = plot_resource_heatmaps(cases, timings_dir)
    if resources is not None:
        generated.append(resources)

    if not skip_timelines:
        for case in cases:
            generated.append(plot_case_timeline(case, timelines_dir))

    generated.append(write_chart_plan(benchmark_path, cases, timings_dir, generated))
    return generated


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate plots and a chart plan from a curated DSLM benchmark JSON file."
    )
    parser.add_argument(
        "benchmark_json",
        nargs="?",
        help="Path to benchmark_run_*.json. Defaults to the latest file under logs/.",
    )
    parser.add_argument(
        "--output-root",
        default=str(DEFAULT_PLOTS_ROOT.relative_to(ROOT)),
        help="Root directory for generated outputs. Default: scripts/plots",
    )
    parser.add_argument(
        "--skip-timelines",
        action="store_true",
        help="Skip per-case timeline plots and only generate the summary figures.",
    )
    return parser


def main() -> int:
    configure_style()
    parser = build_parser()
    args = parser.parse_args()

    benchmark_path = Path(args.benchmark_json) if args.benchmark_json else find_latest_benchmark(DEFAULT_LOG_DIR)
    if not benchmark_path.is_absolute():
        benchmark_path = ROOT / benchmark_path
    if not benchmark_path.exists():
        raise SystemExit(f"Benchmark JSON not found: {benchmark_path}")

    output_root = resolve_output_root(args.output_root)
    generated = generate_plots(benchmark_path, output_root, args.skip_timelines)

    print(f"Input:  {repo_relative(benchmark_path)}")
    print(f"Output: {repo_relative(output_root / benchmark_path.stem)}")
    print("Generated files:")
    for path in generated:
        print(f"  - {repo_relative(path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
