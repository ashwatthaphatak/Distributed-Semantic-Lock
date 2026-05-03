# Author: Ashwattha Phatak
#!/usr/bin/env python3
"""Compare a Qdrant-direct baseline locust run to a full DSLM locust run and
visualise the coordination overhead per persona / operation.

Inputs
------
Two locust ``<prefix>_stats.csv`` files produced by running
``locustfile_base.py`` (baseline) and ``locustfile.py`` (DSLM) with the same
workload knobs.  Request names are expected to follow the schema emitted by
those files:

    QdrantDirect/<label>/<op>      (baseline)
    AcquireGuard/<label>/<op>      (DSLM)

Rows are paired by the trailing ``<label>/<op>`` portion.

Outputs (written to --output-dir)
---------------------------------
Always:
  * ``overhead_summary.csv``    — matched side-by-side numbers with deltas
  * Terminal-printed comparison table
Only if matplotlib + numpy are installed:
  * ``overhead_comparison.png`` — two-panel figure:
      top : grouped bars of baseline vs DSLM p50 latency per endpoint
      bot.: delta bars (DSLM − baseline) for p50, p95, p99
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Optional plotting deps.  Missing these should NOT prevent the CSV + terminal
# summary from being produced.
try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np

    HAS_PLOT_DEPS = True
except ImportError:
    HAS_PLOT_DEPS = False


# Palette aligned with scripts/plot_benchmark_report.py so plots look uniform.
BASELINE_COLOR = "#059669"   # green
DSLM_COLOR = "#2563eb"       # blue
P50_COLOR = "#f59e0b"        # amber
P95_COLOR = "#d97706"        # dark amber
P99_COLOR = "#dc2626"        # red
GRID_COLOR = "#cbd5e1"
TEXT_COLOR = "#0f172a"

PERCENTILE_COLUMNS = [("50%", "p50"), ("95%", "p95"), ("99%", "p99")]


# --------------------------------------------------------------------------- #
# CSV parsing / matching                                                      #
# --------------------------------------------------------------------------- #

def parse_stats_csv(path: Path) -> List[Dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"Input file not found: {path}")
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def extract_key(name: str) -> Optional[str]:
    """Return the ``<label>/<op>`` portion of a Locust request name, or None
    if the row is the aggregate / unknown prefix."""
    for prefix in ("AcquireGuard/", "QdrantDirect/"):
        if name.startswith(prefix):
            return name[len(prefix):]
    return None


def build_index(rows: List[Dict[str, str]]) -> Dict[str, Dict[str, str]]:
    idx: Dict[str, Dict[str, str]] = {}
    for row in rows:
        name = row.get("Name", "") or ""
        key = extract_key(name)
        if key is not None:
            idx[key] = row
    return idx


def to_float(value: Optional[str]) -> float:
    if value is None or value == "":
        return float("nan")
    try:
        return float(value)
    except (TypeError, ValueError):
        return float("nan")


def compute_comparison(
    baseline_rows: List[Dict[str, str]],
    dslm_rows: List[Dict[str, str]],
) -> Tuple[
    List[str],
    Dict[str, Dict[str, str]],
    Dict[str, Dict[str, str]],
    Dict[str, List[float]],
    Dict[str, List[float]],
    Dict[str, List[float]],
]:
    bidx = build_index(baseline_rows)
    didx = build_index(dslm_rows)

    common = sorted(set(bidx) & set(didx))
    if not common:
        raise SystemExit(
            "No matching endpoints found. Make sure both runs used the same "
            "DSCC_AGENT_LABELS and USE_FIRST_PAYLOAD_ONLY settings."
        )

    missing_in_dslm = sorted(set(bidx) - set(didx))
    missing_in_base = sorted(set(didx) - set(bidx))
    if missing_in_dslm:
        print(
            f"[warn] present in baseline but not DSLM: {missing_in_dslm}",
            file=sys.stderr,
        )
    if missing_in_base:
        print(
            f"[warn] present in DSLM but not baseline: {missing_in_base}",
            file=sys.stderr,
        )

    baseline_vals: Dict[str, List[float]] = {}
    dslm_vals: Dict[str, List[float]] = {}
    overhead_vals: Dict[str, List[float]] = {}
    for col, pct in PERCENTILE_COLUMNS:
        baseline_vals[pct] = [to_float(bidx[k].get(col)) for k in common]
        dslm_vals[pct] = [to_float(didx[k].get(col)) for k in common]
        overhead_vals[pct] = [
            d - b for d, b in zip(dslm_vals[pct], baseline_vals[pct])
        ]

    return common, bidx, didx, baseline_vals, dslm_vals, overhead_vals


# --------------------------------------------------------------------------- #
# Output: summary CSV + terminal table (no external deps)                     #
# --------------------------------------------------------------------------- #

def write_summary_csv(
    common: List[str],
    bidx: Dict[str, Dict[str, str]],
    didx: Dict[str, Dict[str, str]],
    baseline_vals: Dict[str, List[float]],
    dslm_vals: Dict[str, List[float]],
    overhead_vals: Dict[str, List[float]],
    out_csv: Path,
) -> None:
    with out_csv.open("w", newline="") as f:
        writer = csv.writer(f)
        header = ["endpoint", "baseline_rps", "dslm_rps"]
        for _, pct in PERCENTILE_COLUMNS:
            header.extend(
                [f"baseline_{pct}_ms", f"dslm_{pct}_ms", f"overhead_{pct}_ms"]
            )
        writer.writerow(header)

        for i, key in enumerate(common):
            row: List[object] = [
                key,
                to_float(bidx[key].get("Requests/s")),
                to_float(didx[key].get("Requests/s")),
            ]
            for _, pct in PERCENTILE_COLUMNS:
                row.extend(
                    [
                        baseline_vals[pct][i],
                        dslm_vals[pct][i],
                        overhead_vals[pct][i],
                    ]
                )
            writer.writerow(row)


def print_terminal_summary(
    common: List[str],
    baseline_vals: Dict[str, List[float]],
    dslm_vals: Dict[str, List[float]],
    overhead_vals: Dict[str, List[float]],
) -> None:
    print()
    print("Overhead summary (p50 / p95 / p99, all ms):")
    print(
        f"  {'endpoint':<14} {'base_p50':>10} {'dslm_p50':>10} "
        f"{'Δp50':>8} {'Δp95':>8} {'Δp99':>8}"
    )
    for i, key in enumerate(common):
        print(
            f"  {key:<14} "
            f"{baseline_vals['p50'][i]:>10.1f} "
            f"{dslm_vals['p50'][i]:>10.1f} "
            f"{overhead_vals['p50'][i]:>8.1f} "
            f"{overhead_vals['p95'][i]:>8.1f} "
            f"{overhead_vals['p99'][i]:>8.1f}"
        )


# --------------------------------------------------------------------------- #
# Output: PNG plot (requires matplotlib + numpy)                              #
# --------------------------------------------------------------------------- #

def _annotate_bars(ax, bars, values, fmt="{:.0f}"):
    for bar, v in zip(bars, values):
        if not np.isfinite(v):
            continue
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height(),
            fmt.format(v),
            ha="center",
            va="bottom",
            fontsize=8,
            color=TEXT_COLOR,
        )


def render_plot(
    common: List[str],
    baseline_vals: Dict[str, List[float]],
    dslm_vals: Dict[str, List[float]],
    overhead_vals: Dict[str, List[float]],
    out_png: Path,
) -> None:
    x = np.arange(len(common))
    width = 0.38
    fig_width = max(10.0, 1.3 * len(common) + 3.0)

    fig, (ax_top, ax_bot) = plt.subplots(
        2, 1, figsize=(fig_width, 10), constrained_layout=True
    )

    # Top panel: side-by-side p50 comparison.
    bars_base = ax_top.bar(
        x - width / 2,
        baseline_vals["p50"],
        width,
        label="Baseline (Qdrant direct)",
        color=BASELINE_COLOR,
        edgecolor="white",
    )
    bars_dslm = ax_top.bar(
        x + width / 2,
        dslm_vals["p50"],
        width,
        label="DSLM (full stack)",
        color=DSLM_COLOR,
        edgecolor="white",
    )
    _annotate_bars(ax_top, bars_base, baseline_vals["p50"])
    _annotate_bars(ax_top, bars_dslm, dslm_vals["p50"])

    ax_top.set_title(
        "p50 response time — Baseline vs DSLM", color=TEXT_COLOR, fontsize=13
    )
    ax_top.set_ylabel("milliseconds", color=TEXT_COLOR)
    ax_top.set_xticks(x)
    ax_top.set_xticklabels(common, rotation=30, ha="right")
    ax_top.grid(axis="y", linestyle="--", color=GRID_COLOR, alpha=0.6)
    ax_top.set_axisbelow(True)
    ax_top.legend(loc="upper left", frameon=False)

    # Bottom panel: overhead deltas for p50 / p95 / p99.
    pct_width = 0.28
    bars_p50 = ax_bot.bar(
        x - pct_width,
        overhead_vals["p50"],
        pct_width,
        label="p50 overhead",
        color=P50_COLOR,
        edgecolor="white",
    )
    bars_p95 = ax_bot.bar(
        x,
        overhead_vals["p95"],
        pct_width,
        label="p95 overhead",
        color=P95_COLOR,
        edgecolor="white",
    )
    bars_p99 = ax_bot.bar(
        x + pct_width,
        overhead_vals["p99"],
        pct_width,
        label="p99 overhead",
        color=P99_COLOR,
        edgecolor="white",
    )
    _annotate_bars(ax_bot, bars_p50, overhead_vals["p50"])
    _annotate_bars(ax_bot, bars_p95, overhead_vals["p95"])
    _annotate_bars(ax_bot, bars_p99, overhead_vals["p99"])

    ax_bot.axhline(0, color=TEXT_COLOR, linewidth=0.8)
    ax_bot.set_title(
        "Coordination overhead (DSLM − Baseline)", color=TEXT_COLOR, fontsize=13
    )
    ax_bot.set_ylabel("milliseconds", color=TEXT_COLOR)
    ax_bot.set_xticks(x)
    ax_bot.set_xticklabels(common, rotation=30, ha="right")
    ax_bot.grid(axis="y", linestyle="--", color=GRID_COLOR, alpha=0.6)
    ax_bot.set_axisbelow(True)
    ax_bot.legend(loc="upper left", frameon=False)

    fig.suptitle(
        "DSLM Coordination Overhead vs Qdrant Baseline",
        fontsize=15,
        fontweight="bold",
        color=TEXT_COLOR,
    )

    fig.savefig(out_png, dpi=150)
    plt.close(fig)


# --------------------------------------------------------------------------- #
# Entry point                                                                 #
# --------------------------------------------------------------------------- #

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compare a Qdrant-direct baseline Locust run to a DSLM "
                    "Locust run and emit a summary CSV (always) and an "
                    "overhead bar chart (if matplotlib is installed)."
    )
    ap.add_argument(
        "--baseline",
        type=Path,
        required=True,
        help="Locust --csv stats file from the direct-Qdrant run "
        "(e.g. logs/overhead_.../baseline_stats.csv)",
    )
    ap.add_argument(
        "--dslm",
        type=Path,
        required=True,
        help="Locust --csv stats file from the full DSLM run",
    )
    ap.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="Directory to write overhead_summary.csv and "
             "overhead_comparison.png.",
    )
    ap.add_argument(
        "--no-plot",
        action="store_true",
        help="Skip PNG generation even if matplotlib is available.",
    )
    args = ap.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    baseline_rows = parse_stats_csv(args.baseline)
    dslm_rows = parse_stats_csv(args.dslm)

    common, bidx, didx, baseline_vals, dslm_vals, overhead_vals = (
        compute_comparison(baseline_rows, dslm_rows)
    )

    # 1. CSV — always
    summary_csv = args.output_dir / "overhead_summary.csv"
    write_summary_csv(
        common, bidx, didx, baseline_vals, dslm_vals, overhead_vals, summary_csv
    )

    # 2. Terminal — always
    print_terminal_summary(common, baseline_vals, dslm_vals, overhead_vals)
    print()
    print(f"Overhead CSV:  {summary_csv}")

    # 3. PNG — only if deps are there
    if args.no_plot:
        print("PNG:           skipped (--no-plot)")
    elif not HAS_PLOT_DEPS:
        print(
            "PNG:           skipped — matplotlib/numpy not installed.\n"
            "               Run `pip install matplotlib numpy` and rerun:\n"
            f"               python3 {Path(__file__).name} "
            f"--baseline {args.baseline} --dslm {args.dslm} "
            f"--output-dir {args.output_dir}"
        )
    else:
        png = args.output_dir / "overhead_comparison.png"
        render_plot(common, baseline_vals, dslm_vals, overhead_vals, png)
        print(f"Overhead plot: {png}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
