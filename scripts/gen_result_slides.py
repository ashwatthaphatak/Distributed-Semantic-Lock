#!/usr/bin/env python3
"""Generate result slide plots for the DSLM final presentation.

Data sourced from MODEL_FINDINGS.md (hardcoded).
Usage:  python3.13 scripts/gen_result_slides.py
Output: scripts/plots/result_slides/
"""

from __future__ import annotations
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

OUT_DIR = Path(__file__).resolve().parent / "plots" / "result_slides"
OUT_DIR.mkdir(parents=True, exist_ok=True)

# ── palette ────────────────────────────────────────────────────────────────────
GRID       = "#cbd5e1"
TEXT       = "#0f172a"
OLLAMA_CLR = "#2563eb"
BGE_CLR    = "#dc2626"
QWEN_CLR   = "#059669"
WINNER_CLR = "#f59e0b"   # amber outline for winner bar

MODEL_LABELS = ["all-minilm", "bge-m3", "qwen3"]
MODEL_COLORS = [OLLAMA_CLR, BGE_CLR, QWEN_CLR]
MODEL_KEYS   = ["ollama", "bge", "qwen"]
THETAS       = [0.55, 0.75, 0.95]
THETA_LABELS = ["θ = 0.55", "θ = 0.75", "θ = 0.95"]

BASE_FONT  = 13
TICK_FONT  = 12
LABEL_FONT = 13
TITLE_FONT = 15
VAL_FONT   = 11


def _style() -> None:
    try:
        plt.style.use("seaborn-v0_8-whitegrid")
    except OSError:
        pass
    plt.rcParams.update({
        "figure.facecolor":  "white",
        "axes.facecolor":    "white",
        "axes.edgecolor":    GRID,
        "axes.labelcolor":   TEXT,
        "axes.titlecolor":   TEXT,
        "xtick.color":       TEXT,
        "ytick.color":       TEXT,
        "grid.color":        GRID,
        "font.size":         BASE_FONT,
        "axes.spines.top":   False,
        "axes.spines.right": False,
    })


# ── raw data ────────────────────────────────────────────────────────────────────

# Case 11 — Paraphrase Gauntlet  (ser_score, violations, op_p95_ms)
C11: dict[str, dict[float, tuple]] = {
    "ollama": {0.55: (0.750, 3, 3031), 0.75: (0.500, 6, 3034), 0.95: (0.833, 2, 3057)},
    "bge":    {0.55: (0.972, 1, 5417), 0.75: (0.750, 3, 3079), 0.95: (0.667, 4, 3074)},
    "qwen":   {0.55: (0.875, 3, 5344), 0.75: (1.000, 0, 3049), 0.95: (0.917, 1, 3049)},
}

# Case 12 — Cross-Domain Flood  (dist_par_rate, op_p95_ms, violations)
C12: dict[str, dict[float, tuple]] = {
    "ollama": {0.55: (0.222, 3069, 1), 0.75: (0.222, 3065, 2), 0.95: (0.250, 3074, 2)},
    "bge":    {0.55: (0.000, 5587, 1), 0.75: (0.278, 3138, 2), 0.95: (0.278, 3036, 1)},
    "qwen":   {0.55: (0.000, 5652, 2), 0.75: (0.306, 3037, 1), 0.95: (0.222, 3103, 2)},
}

# Embedding overhead (ms)
EMB: dict[str, dict[str, float]] = {
    "all-minilm": {"p50": 10,  "p95": 10,  "p99": 324},
    "bge-m3":     {"p50": 111, "p95": 160, "p99": 1774},
    "qwen3":      {"p50": 133, "p95": 402, "p99": 1517},
}

CASE11_PAIRS = 45   # C(10,2)
CASE12_PAIRS = 36   # 6×6 cross-domain


# ── confusion matrix helpers ────────────────────────────────────────────────────

def confusion_at(theta: float) -> dict[str, dict[str, int]]:
    result = {}
    for key in MODEL_KEYS:
        _, viols, _ = C11[key][theta]
        dp_rate, _, _ = C12[key][theta]
        tp = CASE11_PAIRS - viols
        fn = viols
        tn = round(dp_rate * CASE12_PAIRS)
        fp = CASE12_PAIRS - tn
        result[key] = {"TP": tp, "FN": fn, "TN": tn, "FP": fp}
    return result


def confusion_at_075() -> dict[str, dict[str, int]]:
    return confusion_at(0.75)


def prf1(cm: dict[str, int]) -> tuple[float, float, float]:
    tp, fn, fp = cm["TP"], cm["FN"], cm["FP"]
    p = tp / (tp + fp) if (tp + fp) else 0.0
    r = tp / (tp + fn) if (tp + fn) else 0.0
    f = 2 * p * r / (p + r) if (p + r) else 0.0
    return p, r, f


def _bar_labels(ax, bars, vals, fmt="{:.3f}", offset_frac=0.015, ymax=None):
    """Print value above each bar."""
    span = (ymax or ax.get_ylim()[1]) - ax.get_ylim()[0]
    for bar, v in zip(bars, vals):
        label = fmt.format(v) if isinstance(v, float) else str(v)
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() + span * offset_frac,
            label, ha="center", va="bottom",
            fontsize=VAL_FONT, color=TEXT, fontweight="bold",
        )


# ══════════════════════════════════════════════════════════════════════════════
# Plot 1 — Paraphrase Gauntlet: Serialization Score
# ══════════════════════════════════════════════════════════════════════════════

def plot_paraphrase_gauntlet() -> None:
    _style()
    fig, ax = plt.subplots(figsize=(10, 6))

    x = np.arange(len(THETAS))
    w = 0.65 / 3

    for i, (key, label, color) in enumerate(zip(MODEL_KEYS, MODEL_LABELS, MODEL_COLORS)):
        scores = [C11[key][t][0] for t in THETAS]
        bars = ax.bar(x + (i - 1) * w, scores, w, label=label,
                      color=color, edgecolor="white", linewidth=0.8, alpha=0.9)
        _bar_labels(ax, bars, scores, fmt="{:.3f}", offset_frac=0.012, ymax=1.1)

    # Highlight winner bar (qwen @ θ=0.75) with amber outline
    _, _, _ = C11["qwen"][0.75]   # score=1.000
    winner_x_pos = x[1] + 1 * w
    # redraw winner bar on top with outline
    ax.bar([winner_x_pos], [1.000], w,
           color=QWEN_CLR, edgecolor=WINNER_CLR, linewidth=2.5, alpha=0.9)

    ax.axhline(1.0, color=WINNER_CLR, lw=1.5, linestyle="--", alpha=0.6)
    ax.set_ylim(0.4, 1.12)
    ax.set_xticks(x)
    ax.set_xticklabels(THETA_LABELS, fontsize=TICK_FONT)
    ax.set_ylabel("Serialization Score", fontsize=LABEL_FONT)
    ax.set_title("Paraphrase Gauntlet — Serialization Score", fontsize=TITLE_FONT, fontweight="bold", pad=12)
    ax.legend(frameon=False, fontsize=TICK_FONT, loc="lower left")
    ax.grid(axis="y", color=GRID, linestyle="--", alpha=0.7)
    ax.set_axisbelow(True)
    fig.tight_layout()

    out = OUT_DIR / "01_paraphrase_gauntlet_ser_score.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out.name}")


# ══════════════════════════════════════════════════════════════════════════════
# Plot 2 — Violation Heatmap
# ══════════════════════════════════════════════════════════════════════════════

def plot_violation_heatmap() -> None:
    _style()
    fig, ax = plt.subplots(figsize=(7, 4.5))

    data = np.array([[C11[key][t][1] for t in THETAS] for key in MODEL_KEYS], dtype=float)
    im = ax.imshow(data, cmap="RdYlGn_r", vmin=0, vmax=6, aspect="auto")

    for r in range(len(MODEL_KEYS)):
        for c in range(len(THETAS)):
            v = int(data[r, c])
            txt_color = "white" if v >= 4 else TEXT
            ax.text(c, r, str(v), ha="center", va="center",
                    fontsize=20, color=txt_color,
                    fontweight="bold" if v == 0 else "normal")

    # Amber outline on the winning cell (qwen row=2, theta=0.75 col=1)
    from matplotlib.patches import Rectangle
    ax.add_patch(Rectangle((0.52, 1.52), 0.96, 0.96,
                            linewidth=3, edgecolor=WINNER_CLR, facecolor="none",
                            transform=ax.transData))

    ax.set_xticks(range(len(THETAS)))
    ax.set_xticklabels(THETA_LABELS, fontsize=TICK_FONT)
    ax.set_yticks(range(len(MODEL_KEYS)))
    ax.set_yticklabels(MODEL_LABELS, fontsize=TICK_FONT)
    ax.set_xlabel("Similarity Threshold (θ)", fontsize=LABEL_FONT)
    ax.set_title("Paraphrase Gauntlet — Violations", fontsize=TITLE_FONT, fontweight="bold", pad=10)

    cbar = fig.colorbar(im, ax=ax, fraction=0.04, pad=0.02)
    cbar.set_label("Violations", fontsize=TICK_FONT)
    cbar.ax.tick_params(labelsize=TICK_FONT)

    fig.tight_layout()
    out = OUT_DIR / "02_violation_heatmap.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out.name}")


# ══════════════════════════════════════════════════════════════════════════════
# Plot 3 — Confusion Matrices at θ=0.75
# ══════════════════════════════════════════════════════════════════════════════

def plot_confusion_matrices() -> None:
    _style()
    cms = confusion_at_075()
    fig, axes = plt.subplots(1, 3, figsize=(14, 5))
    fig.suptitle("Conflict Detection as Binary Classification  —  θ = 0.75",
                 fontsize=TITLE_FONT, fontweight="bold", color=TEXT, y=1.01)

    for ax, (key, label, color) in zip(axes, zip(MODEL_KEYS, MODEL_LABELS, MODEL_COLORS)):
        cm = cms[key]
        matrix = np.array([[cm["TP"], cm["FN"]],
                            [cm["FP"], cm["TN"]]], dtype=float)
        prec, rec, f1 = prf1(cm)

        ax.imshow(matrix, cmap="Blues", vmin=0, vmax=matrix.sum() * 0.55)

        cell_labels = [
            [f"TP\n{cm['TP']}", f"FN\n{cm['FN']}"],
            [f"FP\n{cm['FP']}", f"TN\n{cm['TN']}"],
        ]
        for r in range(2):
            for c in range(2):
                ax.text(c, r, cell_labels[r][c],
                        ha="center", va="center",
                        fontsize=16, color="white", fontweight="bold")

        ax.set_xticks([0, 1])
        ax.set_xticklabels(["Serialized", "Parallel"], fontsize=TICK_FONT)
        ax.set_yticks([0, 1])
        ax.set_yticklabels(["Should\nconflict", "Should NOT\nconflict"], fontsize=TICK_FONT)
        ax.set_xlabel("Predicted", fontsize=LABEL_FONT, labelpad=6)
        ax.set_ylabel("Actual", fontsize=LABEL_FONT, labelpad=6)
        ax.set_title(f"{label}\nP={prec:.2f}   R={rec:.2f}   F1={f1:.2f}",
                     fontsize=TICK_FONT + 1, fontweight="bold",
                     color=WINNER_CLR if key == "qwen" else TEXT, pad=10)

    fig.tight_layout()
    out = OUT_DIR / "03_confusion_matrices_theta075.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out.name}")


# ══════════════════════════════════════════════════════════════════════════════
# Plot 3b — Confusion Matrices at θ=0.95
# ══════════════════════════════════════════════════════════════════════════════

def plot_confusion_matrices_055() -> None:
    _style()
    cms = confusion_at(0.55)
    fig, axes = plt.subplots(1, 3, figsize=(14, 5))
    fig.suptitle("Conflict Detection as Binary Classification  —  θ = 0.55",
                 fontsize=TITLE_FONT, fontweight="bold", color=TEXT, y=1.01)

    for ax, (key, label, color) in zip(axes, zip(MODEL_KEYS, MODEL_LABELS, MODEL_COLORS)):
        cm = cms[key]
        matrix = np.array([[cm["TP"], cm["FN"]],
                            [cm["FP"], cm["TN"]]], dtype=float)
        prec, rec, f1 = prf1(cm)

        ax.imshow(matrix, cmap="Blues", vmin=0, vmax=matrix.sum() * 0.55)

        cell_labels = [
            [f"TP\n{cm['TP']}", f"FN\n{cm['FN']}"],
            [f"FP\n{cm['FP']}", f"TN\n{cm['TN']}"],
        ]
        for r in range(2):
            for c in range(2):
                ax.text(c, r, cell_labels[r][c],
                        ha="center", va="center",
                        fontsize=16, color="white", fontweight="bold")

        ax.set_xticks([0, 1])
        ax.set_xticklabels(["Serialized", "Parallel"], fontsize=TICK_FONT)
        ax.set_yticks([0, 1])
        ax.set_yticklabels(["Should\nconflict", "Should NOT\nconflict"], fontsize=TICK_FONT)
        ax.set_xlabel("Predicted", fontsize=LABEL_FONT, labelpad=6)
        ax.set_ylabel("Actual", fontsize=LABEL_FONT, labelpad=6)
        ax.set_title(f"{label}\nP={prec:.2f}   R={rec:.2f}   F1={f1:.2f}",
                     fontsize=TICK_FONT + 1, fontweight="bold",
                     color=WINNER_CLR if key == "qwen" else TEXT, pad=10)

    fig.tight_layout()
    out = OUT_DIR / "03c_confusion_matrices_theta055.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out.name}")


def plot_confusion_matrices_095() -> None:
    _style()
    cms = confusion_at(0.95)
    fig, axes = plt.subplots(1, 3, figsize=(14, 5))
    fig.suptitle("Conflict Detection as Binary Classification  —  θ = 0.95",
                 fontsize=TITLE_FONT, fontweight="bold", color=TEXT, y=1.01)

    for ax, (key, label, color) in zip(axes, zip(MODEL_KEYS, MODEL_LABELS, MODEL_COLORS)):
        cm = cms[key]
        matrix = np.array([[cm["TP"], cm["FN"]],
                            [cm["FP"], cm["TN"]]], dtype=float)
        prec, rec, f1 = prf1(cm)

        ax.imshow(matrix, cmap="Blues", vmin=0, vmax=matrix.sum() * 0.55)

        cell_labels = [
            [f"TP\n{cm['TP']}", f"FN\n{cm['FN']}"],
            [f"FP\n{cm['FP']}", f"TN\n{cm['TN']}"],
        ]
        for r in range(2):
            for c in range(2):
                ax.text(c, r, cell_labels[r][c],
                        ha="center", va="center",
                        fontsize=16, color="white", fontweight="bold")

        ax.set_xticks([0, 1])
        ax.set_xticklabels(["Serialized", "Parallel"], fontsize=TICK_FONT)
        ax.set_yticks([0, 1])
        ax.set_yticklabels(["Should\nconflict", "Should NOT\nconflict"], fontsize=TICK_FONT)
        ax.set_xlabel("Predicted", fontsize=LABEL_FONT, labelpad=6)
        ax.set_ylabel("Actual", fontsize=LABEL_FONT, labelpad=6)
        ax.set_title(f"{label}\nP={prec:.2f}   R={rec:.2f}   F1={f1:.2f}",
                     fontsize=TICK_FONT + 1, fontweight="bold",
                     color=WINNER_CLR if key == "qwen" else TEXT, pad=10)

    fig.tight_layout()
    out = OUT_DIR / "03b_confusion_matrices_theta095.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out.name}")


# ══════════════════════════════════════════════════════════════════════════════
# Plot 4 — Precision / Recall / F1 at θ=0.75
# ══════════════════════════════════════════════════════════════════════════════

def plot_prf1() -> None:
    _style()
    cms = confusion_at_075()
    fig, ax = plt.subplots(figsize=(9, 6))

    x = np.arange(3)
    w = 0.22

    for i, (key, label, color) in enumerate(zip(MODEL_KEYS, MODEL_LABELS, MODEL_COLORS)):
        vals = list(prf1(cms[key]))
        bars = ax.bar(x + (i - 1) * w, vals, w, label=label,
                      color=color, edgecolor="white", alpha=0.9)
        _bar_labels(ax, bars, vals, fmt="{:.2f}", offset_frac=0.01, ymax=1.15)

    ax.set_xticks(x)
    ax.set_xticklabels(["Precision", "Recall", "F1"], fontsize=TICK_FONT + 1)
    ax.set_ylim(0, 1.18)
    ax.set_ylabel("Score", fontsize=LABEL_FONT)
    ax.set_title("Classification Metrics at θ = 0.75", fontsize=TITLE_FONT, fontweight="bold", pad=12)
    ax.legend(frameon=False, fontsize=TICK_FONT)
    ax.grid(axis="y", color=GRID, linestyle="--", alpha=0.7)
    ax.set_axisbelow(True)
    fig.tight_layout()

    out = OUT_DIR / "04_precision_recall_f1.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out.name}")


# ══════════════════════════════════════════════════════════════════════════════
# Plot 5 — Cross-Domain Flood: Parallelism Rate + Latency
# ══════════════════════════════════════════════════════════════════════════════

def plot_cross_domain_flood() -> None:
    _style()
    fig, (ax_top, ax_bot) = plt.subplots(2, 1, figsize=(10, 9), sharex=True)
    fig.suptitle("Cross-Domain Flood — Parallelism Rate & Latency",
                 fontsize=TITLE_FONT, fontweight="bold", color=TEXT)

    x = np.arange(len(THETAS))
    w = 0.22

    for i, (key, label, color) in enumerate(zip(MODEL_KEYS, MODEL_LABELS, MODEL_COLORS)):
        rates = [C12[key][t][0] for t in THETAS]
        bars = ax_top.bar(x + (i - 1) * w, rates, w, label=label,
                          color=color, edgecolor="white", alpha=0.9)
        _bar_labels(ax_top, bars, rates, fmt="{:.3f}", offset_frac=0.015, ymax=0.45)

    ax_top.set_ylim(0, 0.48)
    ax_top.set_ylabel("Distinct Parallelism Rate", fontsize=LABEL_FONT)
    ax_top.legend(frameon=False, fontsize=TICK_FONT)
    ax_top.grid(axis="y", color=GRID, linestyle="--", alpha=0.7)
    ax_top.set_axisbelow(True)
    ax_top.set_title("Fraction of unrelated pairs running concurrently  (higher = better)",
                     fontsize=TICK_FONT, color=TEXT)

    for i, (key, label, color) in enumerate(zip(MODEL_KEYS, MODEL_LABELS, MODEL_COLORS)):
        lats = [C12[key][t][1] for t in THETAS]
        bars = ax_bot.bar(x + (i - 1) * w, lats, w, label=label,
                          color=color, edgecolor="white", alpha=0.9)
        _bar_labels(ax_bot, bars, lats, fmt="{:,.0f}", offset_frac=0.008, ymax=6800)

    ax_bot.set_ylim(0, 6800)
    ax_bot.set_ylabel("Op Latency P95 (ms)", fontsize=LABEL_FONT)
    ax_bot.set_xticks(x)
    ax_bot.set_xticklabels(THETA_LABELS, fontsize=TICK_FONT)
    ax_bot.set_title("Operation latency P95", fontsize=TICK_FONT, color=TEXT)
    ax_bot.legend(frameon=False, fontsize=TICK_FONT)
    ax_bot.grid(axis="y", color=GRID, linestyle="--", alpha=0.7)
    ax_bot.set_axisbelow(True)

    fig.tight_layout()
    out = OUT_DIR / "05_cross_domain_flood.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out.name}")


# ══════════════════════════════════════════════════════════════════════════════
# Plot 6 — Embedding Overhead
# ══════════════════════════════════════════════════════════════════════════════

def plot_embedding_overhead() -> None:
    _style()
    fig, ax = plt.subplots(figsize=(9, 6))

    emb_models  = list(EMB.keys())
    percentiles = ["p50", "p95", "p99"]
    pct_colors  = ["#2563eb", "#d97706", "#dc2626"]

    x  = np.arange(len(emb_models))
    w  = 0.22

    for j, (pct, pcol) in enumerate(zip(percentiles, pct_colors)):
        vals = [EMB[m][pct] for m in emb_models]
        bars = ax.bar(x + (j - 1) * w, vals, w, label=pct,
                      color=pcol, edgecolor="white", alpha=0.9)
        _bar_labels(ax, bars, vals, fmt="{:,.0f}", offset_frac=0.008, ymax=1900)

    ax.set_xticks(x)
    ax.set_xticklabels(
        ["all-minilm\n(384-dim, 23M params)", "bge-m3\n(1024-dim)", "qwen3\n(1024-dim, 0.6B params)"],
        fontsize=TICK_FONT,
    )
    ax.set_ylabel("Embedding Latency (ms)", fontsize=LABEL_FONT)
    ax.set_title("Embedding Overhead per AcquireGuard Call", fontsize=TITLE_FONT, fontweight="bold", pad=12)
    ax.legend(frameon=False, fontsize=TICK_FONT)
    ax.grid(axis="y", color=GRID, linestyle="--", alpha=0.7)
    ax.set_axisbelow(True)
    fig.tight_layout()

    out = OUT_DIR / "06_embedding_overhead.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out.name}")


# ══════════════════════════════════════════════════════════════════════════════
# Plot 7 — Model Summary: 4 key metrics at θ=0.75
# ══════════════════════════════════════════════════════════════════════════════

def plot_summary() -> None:
    _style()
    cms = confusion_at_075()

    panels = [
        ("Serialization Score",   [C11[k][0.75][0] for k in MODEL_KEYS], "{:.3f}", 0, 1.08),
        ("Violations",            [C11[k][0.75][1] for k in MODEL_KEYS], "{:d}",   0, 7.5),
        ("Distinct Parallelism Rate", [C12[k][0.75][0] for k in MODEL_KEYS], "{:.3f}", 0, 0.42),
        ("Embedding p50 (ms)",    [EMB[m]["p50"] for m in ["all-minilm", "bge-m3", "qwen3"]], "{:g}", 0, 160),
    ]

    fig, axes = plt.subplots(1, 4, figsize=(15, 5.5))
    fig.suptitle("Model Comparison at θ = 0.75",
                 fontsize=TITLE_FONT + 1, fontweight="bold", color=TEXT, y=1.02)

    for ax, (title, vals, fmt, ymin, ymax) in zip(axes, panels):
        bars = ax.bar(range(3), vals, color=MODEL_COLORS, edgecolor="white", alpha=0.9)
        _bar_labels(ax, bars, vals, fmt=fmt, offset_frac=0.015, ymax=ymax)
        ax.set_xticks(range(3))
        ax.set_xticklabels(["all-\nminilm", "bge-\nm3", "qwen3"], fontsize=TICK_FONT)
        ax.set_ylim(ymin, ymax)
        ax.set_title(title, fontsize=TICK_FONT + 1, fontweight="bold", color=TEXT, pad=8)
        ax.grid(axis="y", color=GRID, linestyle="--", alpha=0.6)
        ax.set_axisbelow(True)

        # Amber outline on the best bar
        # Ser. score / distinct par. rate → qwen (idx 2) is best
        # Violations / embedding p50 → ollama (idx 0) is best
        if title in ("Violations", "Embedding p50 (ms)"):
            best_idx = int(np.argmin(vals))
        else:
            best_idx = int(np.argmax(vals))
        bars[best_idx].set_edgecolor(WINNER_CLR)
        bars[best_idx].set_linewidth(2.5)

    patches = [mpatches.Patch(color=c, label=l)
               for c, l in zip(MODEL_COLORS, MODEL_LABELS)]
    fig.legend(handles=patches, loc="lower center", ncol=3,
               frameon=False, fontsize=TICK_FONT, bbox_to_anchor=(0.5, -0.04))

    fig.tight_layout()
    out = OUT_DIR / "07_model_summary_theta075.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out.name}")


# ══════════════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    print(f"Generating result slide plots -> {OUT_DIR}")
    plot_paraphrase_gauntlet()
    plot_violation_heatmap()
    plot_confusion_matrices()
    plot_confusion_matrices_055()
    plot_confusion_matrices_095()
    plot_prf1()
    plot_cross_domain_flood()
    plot_embedding_overhead()
    plot_summary()
    print("Done.")
