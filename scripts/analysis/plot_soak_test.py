# Author: Ayush Gala
#!/usr/bin/env python3
"""Plot time-series latency data from a DSLM soak test CSV.

Usage:
  python3 scripts/plot_soak_test.py                     # auto-detect latest soak_run_*.csv
  python3 scripts/plot_soak_test.py logs/soak_run_*.csv # explicit path
"""

import sys
import pathlib
import glob

import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLOTS_DIR = ROOT / "scripts" / "plots" / "soak"


def find_latest_csv() -> pathlib.Path:
    candidates = sorted(
        (ROOT / "logs").glob("soak_run_*.csv"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    if not candidates:
        raise FileNotFoundError("No soak_run_*.csv found in logs/")
    return candidates[0]


def load_csv(path: pathlib.Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    df["elapsed_min"] = df["elapsed_sec"] / 60.0
    return df


def has_data(series: pd.Series) -> bool:
    return series.notna().any() and (series > 0).any()


def save(fig: plt.Figure, name: str) -> None:
    PLOTS_DIR.mkdir(parents=True, exist_ok=True)
    out = PLOTS_DIR / name
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out}")


# ---------------------------------------------------------------------------
# Individual plot functions
# ---------------------------------------------------------------------------

def plot_lock_wait_latency(df: pd.DataFrame) -> None:
    """Lock-wait percentiles over time — the key fairness/serialisation metric."""
    if not has_data(df["lock_wait_p95_ms"]):
        print("  [skip] lock_wait_latency — no data")
        return
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(df["elapsed_min"], df["lock_wait_p50_ms"], label="lock_wait p50", linewidth=1.5)
    ax.plot(df["elapsed_min"], df["lock_wait_p95_ms"], label="lock_wait p95", linewidth=1.5)
    ax.plot(df["elapsed_min"], df["lock_wait_p99_ms"], label="lock_wait p99",
            linewidth=1.5, linestyle="--")
    ax.set_xlabel("Elapsed (min)")
    ax.set_ylabel("Lock-wait latency (ms)")
    ax.set_title("Lock-Wait Latency vs. DB Population\n"
                 "(flat line = serialisation is DB-independent)")
    ax.legend()
    ax.grid(True, alpha=0.3)
    save(fig, "soak_01_lock_wait.png")


def plot_op_latency(df: pd.DataFrame) -> None:
    """End-to-end operation latency percentiles over time."""
    if not has_data(df["op_latency_p95_ms"]):
        print("  [skip] op_latency — no data")
        return
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(df["elapsed_min"], df["op_latency_p50_ms"], label="op latency p50", linewidth=1.5)
    ax.plot(df["elapsed_min"], df["op_latency_p95_ms"], label="op latency p95", linewidth=1.5)
    ax.plot(df["elapsed_min"], df["op_latency_p99_ms"], label="op latency p99",
            linewidth=1.5, linestyle="--")
    ax.set_xlabel("Elapsed (min)")
    ax.set_ylabel("Operation latency (ms)")
    ax.set_title("End-to-End Operation Latency Over Time")
    ax.legend()
    ax.grid(True, alpha=0.3)
    save(fig, "soak_02_op_latency.png")


def plot_qdrant_window(df: pd.DataFrame) -> None:
    """Qdrant upsert window P95 over time — only metric that SHOULD grow with DB size."""
    if not has_data(df["qdrant_window_p95_ms"]):
        print("  [skip] qdrant_window — no data")
        return
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(df["elapsed_min"], df["qdrant_window_p95_ms"],
            color="tab:orange", linewidth=1.5, label="qdrant upsert window p95")
    ax.set_xlabel("Elapsed (min)")
    ax.set_ylabel("Qdrant upsert window (ms)")
    ax.set_title("Qdrant Upsert Window P95 Over Time\n"
                 "(may grow as collection fills; lock_wait should not)")
    ax.legend()
    ax.grid(True, alpha=0.3)
    save(fig, "soak_03_qdrant_window.png")


def plot_qdrant_size(df: pd.DataFrame) -> None:
    """Collection size growth over time — confirms the DB is actually being populated."""
    if not has_data(df["qdrant_size"]):
        print("  [skip] qdrant_size — no data")
        return
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(df["elapsed_min"], df["qdrant_size"],
            color="tab:purple", linewidth=1.5, label="Qdrant collection size")
    ax.set_xlabel("Elapsed (min)")
    ax.set_ylabel("Collection size (points)")
    ax.set_title("Qdrant Collection Size Growth")
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: f"{int(x):,}"))
    ax.legend()
    ax.grid(True, alpha=0.3)
    save(fig, "soak_04_qdrant_size.png")


def plot_blocked_rate(df: pd.DataFrame) -> None:
    """Fraction of operations that waited for a semantic lock."""
    if not has_data(df["blocked_rate"]):
        print("  [skip] blocked_rate — no data")
        return
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(df["elapsed_min"], df["blocked_rate"] * 100,
            color="tab:red", linewidth=1.5, label="blocked %")
    ax.set_xlabel("Elapsed (min)")
    ax.set_ylabel("Blocked rate (%)")
    ax.set_title("Serialisation Pressure Over Time\n"
                 "(stable % = consistent conflict detection regardless of DB size)")
    ax.legend()
    ax.grid(True, alpha=0.3)
    save(fig, "soak_05_blocked_rate.png")


def plot_throughput(df: pd.DataFrame) -> None:
    """Operations per second over time."""
    if not has_data(df["throughput_ops_per_sec"]):
        print("  [skip] throughput — no data")
        return
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(df["elapsed_min"], df["throughput_ops_per_sec"],
            color="tab:green", linewidth=1.5, label="throughput (ops/s)")
    ax.set_xlabel("Elapsed (min)")
    ax.set_ylabel("Throughput (ops/s)")
    ax.set_title("System Throughput Over Time")
    ax.legend()
    ax.grid(True, alpha=0.3)
    save(fig, "soak_06_throughput.png")


def plot_combined_comparison(df: pd.DataFrame) -> None:
    """Side-by-side: lock_wait_p95 vs qdrant_window_p95 vs qdrant_size.

    This is the headline plot: it shows the two latency components against
    the growing collection, letting you verify that only Qdrant write cost
    scales with DB size while the serialisation path stays flat.
    """
    if not (has_data(df["lock_wait_p95_ms"]) or has_data(df["qdrant_window_p95_ms"])):
        print("  [skip] combined_comparison — no data")
        return
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 9), sharex=True)

    ax1.plot(df["elapsed_min"], df["lock_wait_p95_ms"],
             label="lock_wait p95 (serialisation)", color="tab:blue", linewidth=2)
    ax1.plot(df["elapsed_min"], df["qdrant_window_p95_ms"],
             label="qdrant upsert window p95 (persistence)", color="tab:orange", linewidth=2)
    ax1.set_ylabel("Latency (ms)")
    ax1.set_title("Lock Wait vs. Qdrant Upsert Window as Collection Grows")
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    ax2_l = ax2
    ax2_r = ax2.twinx()
    ax2_l.fill_between(df["elapsed_min"], df["qdrant_size"],
                       alpha=0.25, color="tab:purple", label="qdrant collection size")
    ax2_l.set_xlabel("Elapsed (min)")
    ax2_l.set_ylabel("Collection size (points)", color="tab:purple")
    ax2_l.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: f"{int(x):,}"))
    ax2_l.tick_params(axis="y", labelcolor="tab:purple")

    if has_data(df["blocked_rate"]):
        ax2_r.plot(df["elapsed_min"], df["blocked_rate"] * 100,
                   color="tab:red", linewidth=1.5, linestyle="--", label="blocked %")
        ax2_r.set_ylabel("Blocked rate (%)", color="tab:red")
        ax2_r.tick_params(axis="y", labelcolor="tab:red")
        lines2 = ax2_r.get_lines()
        labels2 = [l.get_label() for l in lines2]
        handles1, labels1 = ax2_l.get_legend_handles_labels()
        ax2.legend(handles1 + lines2, labels1 + labels2, loc="upper left")

    fig.tight_layout()
    save(fig, "soak_07_combined.png")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    if len(sys.argv) > 1:
        csv_path = pathlib.Path(sys.argv[1])
    else:
        csv_path = find_latest_csv()

    print(f"Loading {csv_path}")
    df = load_csv(csv_path)
    print(f"  {len(df)} snapshots  "
          f"duration={df['elapsed_min'].max():.1f} min  "
          f"total_ops={df['total_ops'].max()}")

    print("Generating plots →", PLOTS_DIR)
    plot_lock_wait_latency(df)
    plot_op_latency(df)
    plot_qdrant_window(df)
    plot_qdrant_size(df)
    plot_blocked_rate(df)
    plot_throughput(df)
    plot_combined_comparison(df)
    print("Done.")


if __name__ == "__main__":
    main()
