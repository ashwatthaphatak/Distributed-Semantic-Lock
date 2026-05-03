# Author: Ashwattha Phatak
#!/usr/bin/env python3
"""Generate comparison bar charts from a DSLM matrix benchmark CSV.

The matrix CSV has one row per (profile × theta × scenario) combination.
All plots use theta as the categorical x-axis with one grouped bar per model,
so the effect of threshold choice is always visible alongside model differences.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
from pathlib import Path
from typing import Any

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch
    import numpy as np
except ImportError as exc:
    raise SystemExit(
        "This script requires matplotlib and numpy in the local Python environment."
    ) from exc


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG_DIR = ROOT / "logs"
DEFAULT_PLOTS_ROOT = ROOT / "scripts" / "plots" / "matrix"

# --- colour palette -----------------------------------------------------------

GRID = "#cbd5e1"
TEXT = "#0f172a"

MODEL_COLORS = {
    "ollama": "#2563eb",
    "bge":    "#dc2626",
    "qwen":   "#059669",
}
DEFAULT_COLORS = ["#2563eb", "#dc2626", "#059669", "#d97706", "#7c3aed"]

LATENCY_COLOR  = "#2563eb"
WAIT_COLOR     = "#f59e0b"
FAIR_COLOR     = "#059669"
BLOCK_COLOR    = "#dc2626"
EMB_COLOR      = "#7c3aed"

RESEARCH_CASES = {
    "The Paraphrase Gauntlet":     11,
    "The Cross-Domain Flood":      12,
    "The Write Pressure Ratchet":  13,
}


# --- style --------------------------------------------------------------------


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


def model_color(profile: str, index: int) -> str:
    return MODEL_COLORS.get(profile, DEFAULT_COLORS[index % len(DEFAULT_COLORS)])


def short_model(model_id: str) -> str:
    name = model_id.split("/")[-1]
    return re.sub(r":latest$", "", name)


# --- data loading -------------------------------------------------------------


def normalise_theta(raw: str) -> str:
    try:
        return f"{float(raw):.3f}"
    except ValueError:
        return raw.strip()


def load_matrix(path: Path) -> list[dict[str, Any]]:
    """Load matrix CSV, deduplicating by (profile, theta, scenario).

    If a (profile, theta, scenario) tuple appears multiple times (e.g. after a
    crash-and-restart appended duplicate rows), keep the last occurrence so the
    most-recent run's metrics are used.
    """
    seen: dict[tuple[str, str, str], int] = {}
    rows: list[dict[str, Any]] = []
    with path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            row["theta"] = normalise_theta(row.get("theta", "0"))
            key = (row.get("profile", ""), row["theta"], row.get("scenario", ""))
            if key in seen:
                rows[seen[key]] = row  # overwrite with later run
            else:
                seen[key] = len(rows)
                rows.append(row)
    return rows


def find_latest_matrix(log_dir: Path) -> Path:
    candidates = sorted(log_dir.glob("*_matrix.csv"), key=lambda p: p.stat().st_mtime)
    if not candidates:
        raise SystemExit(f"No *_matrix.csv files found under {log_dir}")
    return candidates[-1]


def float_val(row: dict[str, Any], key: str) -> float:
    try:
        v = float(row.get(key, 0) or 0)
        return v if math.isfinite(v) else 0.0
    except (ValueError, TypeError):
        return 0.0


# --- aggregation helpers ------------------------------------------------------


def profiles_ordered(rows: list[dict[str, Any]]) -> list[str]:
    seen: dict[str, int] = {}
    for row in rows:
        p = row.get("profile", "")
        if p not in seen:
            seen[p] = len(seen)
    order = {"ollama": 0, "bge": 1, "qwen": 2}
    return sorted(seen.keys(), key=lambda p: order.get(p, 99))


def thetas_ordered(rows: list[dict[str, Any]]) -> list[str]:
    seen: dict[str, None] = {}
    for row in rows:
        seen[row["theta"]] = None
    return sorted(seen.keys(), key=float)


def aggregate(
    rows: list[dict[str, Any]],
    metric: str,
    profile: str,
    theta: str,
    scenario_filter: str | None = None,
) -> float:
    subset = [
        r for r in rows
        if r.get("profile") == profile
        and r["theta"] == theta
        and (scenario_filter is None or r.get("scenario") == scenario_filter)
    ]
    if not subset:
        return 0.0
    values = [float_val(r, metric) for r in subset]
    return sum(values) / len(values)


def has_data(values: list[float]) -> bool:
    return any(v not in (0.0, float("nan")) and math.isfinite(v) for v in values)


# --- shared bar chart helper --------------------------------------------------


def grouped_bar_chart(
    ax: Any,
    thetas: list[str],
    profiles: list[str],
    values_by_profile: dict[str, list[float]],
    ylabel: str,
    title: str,
) -> None:
    n_thetas = len(thetas)
    n_profiles = len(profiles)
    bar_width = 0.8 / n_profiles
    x = np.arange(n_thetas)

    for pi, profile in enumerate(profiles):
        offset = (pi - n_profiles / 2.0 + 0.5) * bar_width
        color = model_color(profile, pi)
        model_vals = values_by_profile[profile]
        ax.bar(
            x + offset,
            model_vals,
            width=bar_width * 0.9,
            color=color,
            label=profile,
            edgecolor="white",
        )

    ax.set_title(title)
    ax.set_ylabel(ylabel)
    ax.set_xticks(x)
    ax.set_xticklabels([f"θ={t}" for t in thetas])
    ax.grid(True, axis="y", linestyle="--", linewidth=0.6, alpha=0.7)
    ax.legend(frameon=True, fontsize=8)


# --- plot functions -----------------------------------------------------------


def plot_latency_by_theta(
    rows: list[dict[str, Any]],
    profiles: list[str],
    thetas: list[str],
    output_dir: Path,
) -> Path | None:
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))

    for ax, metric, title, ylabel in (
        (axes[0], "latency_p95_ms",   "End-to-End Latency P95 by Theta",  "Milliseconds"),
        (axes[1], "lock_wait_p95_ms", "Lock Wait P95 by Theta",            "Milliseconds"),
    ):
        vals = {
            p: [aggregate(rows, metric, p, t) for t in thetas]
            for p in profiles
        }
        if not has_data([v for vlist in vals.values() for v in vlist]):
            plt.close(fig)
            return None
        grouped_bar_chart(ax, thetas, profiles, vals, ylabel, title)

    fig.suptitle("Latency vs Threshold — All Scenarios", fontsize=14, y=1.01)
    fig.tight_layout()
    path = output_dir / "matrix_01_latency_by_theta.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_fairness_by_theta(
    rows: list[dict[str, Any]],
    profiles: list[str],
    thetas: list[str],
    output_dir: Path,
) -> Path | None:
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))

    for ax, metric, title, ylabel in (
        (axes[0], "serialization_score",     "Serialization Score by Theta\n(1.0 = zero violations)", "Score (0–1)"),
        (axes[1], "distinct_parallelism_rate","Distinct Parallelism Rate by Theta\n(1.0 = fully parallel)", "Rate (0–1)"),
    ):
        vals = {
            p: [aggregate(rows, metric, p, t) for t in thetas]
            for p in profiles
        }
        if not has_data([v for vlist in vals.values() for v in vlist]):
            continue
        grouped_bar_chart(ax, thetas, profiles, vals, ylabel, title)
        ax.set_ylim(0.0, 1.05)

    fig.suptitle("Fairness Metrics vs Threshold — All Scenarios", fontsize=14, y=1.01)
    fig.tight_layout()
    path = output_dir / "matrix_02_fairness_by_theta.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_blocked_rate_by_theta(
    rows: list[dict[str, Any]],
    profiles: list[str],
    thetas: list[str],
    output_dir: Path,
) -> Path | None:
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))

    for ax, metric, title, ylabel in (
        (axes[0], "blocked_rate",  "Blocked Op Rate by Theta\n(fraction of ops that waited)", "Fraction"),
        (axes[1], "blocked_ops",   "Blocked Op Count by Theta\n(total across all scenarios)",  "Count"),
    ):
        vals = {
            p: [aggregate(rows, metric, p, t) for t in thetas]
            for p in profiles
        }
        if not has_data([v for vlist in vals.values() for v in vlist]):
            continue
        grouped_bar_chart(ax, thetas, profiles, vals, ylabel, title)

    fig.suptitle("Blocking Behaviour vs Threshold", fontsize=14, y=1.01)
    fig.tight_layout()
    path = output_dir / "matrix_03_blocked_rate_by_theta.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_embedding_overhead(
    rows: list[dict[str, Any]],
    profiles: list[str],
    output_dir: Path,
) -> Path | None:
    # Embedding latency is model-specific (constant across thetas); average over all rows.
    stats: dict[str, dict[str, float]] = {}
    for profile in profiles:
        subset = [r for r in rows if r.get("profile") == profile]
        if not subset:
            continue
        stats[profile] = {
            "p50": sum(float_val(r, "embedding_p50_ms") for r in subset) / len(subset),
            "p95": sum(float_val(r, "embedding_p95_ms") for r in subset) / len(subset),
            "p99": sum(float_val(r, "embedding_p99_ms") for r in subset) / len(subset),
        }

    if not stats or not has_data([s["p50"] for s in stats.values()]):
        return None

    n = len(profiles)
    x = np.arange(n)
    width = 0.25
    x_labels = [p for p in profiles if p in stats]
    p50_vals = [stats[p]["p50"] for p in x_labels]
    p95_vals = [stats[p]["p95"] for p in x_labels]
    p99_vals = [stats[p]["p99"] for p in x_labels]

    fig, ax = plt.subplots(figsize=(10, 6))
    ax.bar(x - width, p50_vals, width=width * 0.9, color=EMB_COLOR, alpha=0.9, label="p50")
    ax.bar(x,         p95_vals, width=width * 0.9, color=EMB_COLOR, alpha=0.65, label="p95")
    ax.bar(x + width, p99_vals, width=width * 0.9, color=EMB_COLOR, alpha=0.4, label="p99")
    ax.set_xticks(x[:len(x_labels)])
    ax.set_xticklabels(x_labels)
    ax.set_ylabel("Milliseconds")
    ax.set_title("Embedding Overhead by Model\n(measured during template pre-embedding)")
    ax.legend(frameon=True)
    ax.grid(True, axis="y", linestyle="--", linewidth=0.6, alpha=0.7)
    fig.tight_layout()
    path = output_dir / "matrix_04_embedding_overhead.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_latency_heatmap(
    rows: list[dict[str, Any]],
    profiles: list[str],
    thetas: list[str],
    output_dir: Path,
) -> Path | None:
    metric = "latency_p95_ms"
    matrix = np.zeros((len(profiles), len(thetas)))
    for pi, profile in enumerate(profiles):
        for ti, theta in enumerate(thetas):
            matrix[pi, ti] = aggregate(rows, metric, profile, theta)

    if not has_data(matrix.flatten().tolist()):
        return None

    fig, ax = plt.subplots(figsize=(9, 5))
    im = ax.imshow(matrix, aspect="auto", cmap="YlOrRd")
    ax.set_xticks(np.arange(len(thetas)))
    ax.set_xticklabels([f"θ={t}" for t in thetas])
    ax.set_yticks(np.arange(len(profiles)))
    ax.set_yticklabels(profiles)
    for pi in range(len(profiles)):
        for ti in range(len(thetas)):
            ax.text(ti, pi, f"{matrix[pi, ti]:.0f}",
                    ha="center", va="center", fontsize=9, color="black")
    fig.colorbar(im, ax=ax, label="latency_p95_ms")
    ax.set_title("Latency P95 Heatmap (model × theta, avg over all scenarios)")
    fig.tight_layout()
    path = output_dir / "matrix_05_latency_heatmap.png"
    fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return path


def _research_case_plot(
    rows: list[dict[str, Any]],
    profiles: list[str],
    thetas: list[str],
    scenario_name: str,
    metric_pairs: list[tuple[str, str, str]],
    suptitle: str,
    output_path: Path,
) -> Path | None:
    scenario_rows = [r for r in rows if r.get("scenario") == scenario_name]
    if not scenario_rows:
        return None

    n_panels = len(metric_pairs)
    fig, axes = plt.subplots(1, n_panels, figsize=(7 * n_panels, 6))
    if n_panels == 1:
        axes = [axes]

    any_data = False
    for ax, (metric, title, ylabel) in zip(axes, metric_pairs):
        vals = {
            p: [aggregate(scenario_rows, metric, p, t) for t in thetas]
            for p in profiles
        }
        if not has_data([v for vlist in vals.values() for v in vlist]):
            continue
        any_data = True
        grouped_bar_chart(ax, thetas, profiles, vals, ylabel, title)
        if "score" in metric or "rate" in metric:
            ax.set_ylim(0.0, 1.05)

    if not any_data:
        plt.close(fig)
        return None

    fig.suptitle(suptitle, fontsize=13, y=1.01)
    fig.tight_layout()
    fig.savefig(output_path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    return output_path


def plot_case11_paraphrase(
    rows: list[dict[str, Any]],
    profiles: list[str],
    thetas: list[str],
    output_dir: Path,
) -> Path | None:
    return _research_case_plot(
        rows, profiles, thetas,
        scenario_name="The Paraphrase Gauntlet",
        metric_pairs=[
            ("serialization_score",          "Serialization Score\n(1.0 = zero violations)", "Score (0–1)"),
            ("latency_p95_ms",               "Latency P95",                                  "Milliseconds"),
        ],
        suptitle="Case 11 — The Paraphrase Gauntlet\nCorrectness under paraphrase: do all models detect near-duplicate phrasing?",
        output_path=output_dir / "matrix_06_case11_paraphrase.png",
    )


def plot_case12_cross_domain(
    rows: list[dict[str, Any]],
    profiles: list[str],
    thetas: list[str],
    output_dir: Path,
) -> Path | None:
    return _research_case_plot(
        rows, profiles, thetas,
        scenario_name="The Cross-Domain Flood",
        metric_pairs=[
            ("distinct_parallelism_rate",    "Distinct Parallelism Rate\n(1.0 = no false-positive blocking)", "Rate (0–1)"),
            ("latency_p95_ms",               "Latency P95\n(lower is better — no false blocking)",           "Milliseconds"),
        ],
        suptitle="Case 12 — The Cross-Domain Flood\nSpecificity: do weak models incorrectly block unrelated domains?",
        output_path=output_dir / "matrix_07_case12_cross_domain.png",
    )


def plot_case13_write_pressure(
    rows: list[dict[str, Any]],
    profiles: list[str],
    thetas: list[str],
    output_dir: Path,
) -> Path | None:
    return _research_case_plot(
        rows, profiles, thetas,
        scenario_name="The Write Pressure Ratchet",
        metric_pairs=[
            ("write_latency_p95_ms",  "Write Latency P95",  "Milliseconds"),
            ("read_latency_p95_ms",   "Read Latency P95\n(fairness cost for readers)", "Milliseconds"),
        ],
        suptitle="Case 13 — The Write Pressure Ratchet\nFairness: read latency vs write latency under sustained write load",
        output_path=output_dir / "matrix_08_case13_write_pressure.png",
    )


# --- orchestration ------------------------------------------------------------


def generate_plots(
    csv_path: Path,
    output_dir: Path,
    skip_per_case: bool,
) -> list[Path]:
    rows = load_matrix(csv_path)
    if not rows:
        raise SystemExit(f"No rows found in {csv_path}")

    profiles = profiles_ordered(rows)
    thetas = thetas_ordered(rows)

    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Profiles : {profiles}")
    print(f"Thetas   : {thetas}")
    print(f"Rows     : {len(rows)}")
    print(f"Output   : {output_dir}")

    generated: list[Path] = []

    def maybe_add(path: Path | None) -> None:
        if path is not None:
            generated.append(path)

    maybe_add(plot_latency_by_theta(rows, profiles, thetas, output_dir))
    maybe_add(plot_fairness_by_theta(rows, profiles, thetas, output_dir))
    maybe_add(plot_blocked_rate_by_theta(rows, profiles, thetas, output_dir))
    maybe_add(plot_embedding_overhead(rows, profiles, output_dir))
    maybe_add(plot_latency_heatmap(rows, profiles, thetas, output_dir))

    if not skip_per_case:
        maybe_add(plot_case11_paraphrase(rows, profiles, thetas, output_dir))
        maybe_add(plot_case12_cross_domain(rows, profiles, thetas, output_dir))
        maybe_add(plot_case13_write_pressure(rows, profiles, thetas, output_dir))

    return generated


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Generate comparison bar charts from a DSLM matrix benchmark CSV. "
            "Reads the latest *_matrix.csv from logs/ by default."
        )
    )
    parser.add_argument(
        "matrix_csv",
        nargs="?",
        help="Path to the matrix CSV file. Defaults to the latest *_matrix.csv under logs/.",
    )
    parser.add_argument(
        "--output-dir",
        default=str(DEFAULT_PLOTS_ROOT),
        help=f"Output directory for generated plots. Default: {DEFAULT_PLOTS_ROOT}",
    )
    parser.add_argument(
        "--skip-per-case",
        action="store_true",
        help="Skip the three research case plots (cases 11-13).",
    )
    return parser


def main() -> int:
    configure_style()
    args = build_parser().parse_args()

    csv_path = (
        Path(args.matrix_csv)
        if args.matrix_csv
        else find_latest_matrix(DEFAULT_LOG_DIR)
    )
    if not csv_path.is_absolute():
        csv_path = ROOT / csv_path
    if not csv_path.exists():
        raise SystemExit(f"Matrix CSV not found: {csv_path}")

    output_dir = Path(args.output_dir)
    generated = generate_plots(csv_path, output_dir, args.skip_per_case)

    print("Generated files:")
    for p in generated:
        print(f"  - {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
