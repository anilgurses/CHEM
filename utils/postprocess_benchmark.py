#!/usr/bin/env python3
"""
Post-process CHEM benchmark suite results.

Expects a directory of CSVs from run_benchmark_suite.sh with filenames like:
    srLTE-10MHz_inj1000us_cir1.csv

Usage:
    python postprocess_benchmark.py benchmark_results_20260301/
    python postprocess_benchmark.py benchmark_results_20260301/ -o figures/ --formats pdf eps
"""

import argparse
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from matplotlib.lines import Line2D
import numpy as np
import pandas as pd

COLORS = ["#4477AA", "#EE6677", "#228833", "#CCBB44", "#66CCEE", "#AA3377"]
MARKERS = ["o", "s", "^", "D", "v", "P"]
LINESTYLES = ["-", "--", "-.", ":"]
LEGEND_FONT_SMALL = 7
LEGEND_TITLE_FONT_SMALL = 8
LEGEND_COL_WIDTH_SINGLE = 1.30
LEGEND_COL_WIDTH_COMBINED = 1.40
LEGEND_COL_WSPACE = 0.01
TABLE_CHANNEL_COUNT = 1
# Saturation filtering: drop-rate thresholds (%).
# Latency plots use a tight threshold because E2E latency hits the deadline
# cap well before 80% drop rate, making those data points meaningless.
SATURATED_DROP_PCT = 80        # throughput, drop-rate, resources
LATENCY_DROP_PCT = 10          # latency, CDF, percentile bars

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
    "font.size": 10,
    "axes.labelsize": 11,
    "axes.titlesize": 11,
    "legend.fontsize": 8,
    "xtick.labelsize": 9,
    "ytick.labelsize": 9,
    "figure.dpi": 300,
    "savefig.dpi": 300,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.05,
    "axes.grid": True,
    "grid.alpha": 0.25,
    "grid.linewidth": 0.4,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "lines.linewidth": 1.4,
    "lines.markersize": 4.5,
    "figure.figsize": (3.5, 2.6),
})


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _c(i):
    return COLORS[i % len(COLORS)]


def _m(i):
    return MARKERS[i % len(MARKERS)]


def _ls(i):
    return LINESTYLES[i % len(LINESTYLES)]


def _save(fig, out_dir: Path, name: str, fmts: list[str]):
    for fmt in fmts:
        fig.savefig(out_dir / f"{name}.{fmt}", format=fmt)
    plt.close(fig)


def _link_count(df: pd.DataFrame) -> pd.Series:
    return df["tx_count"] * df["rx_count"]


def _has_percentiles(df: pd.DataFrame) -> bool:
    return "latency_total_p50_us" in df.columns


def _has_detailed(df: pd.DataFrame) -> bool:
    return "latency_resample_avg_us" in df.columns


def _filter_saturated(df: pd.DataFrame,
                      threshold: float = SATURATED_DROP_PCT) -> pd.DataFrame:
    """Filter out rows where drop rate >= *threshold* %."""
    if "drop_rate_pct" in df.columns:
        return df[df["drop_rate_pct"] < threshold].copy()
    return df


def _select_indices(n: int) -> list[int]:
    """Pick ~5 evenly-spaced row indices for tables / CDF plots."""
    indices = sorted(set([0, n // 4, n // 2, 3 * n // 4, n - 1]))
    return [i for i in indices if i < n]


def _fmt(val, prec=2):
    return "--" if val == 0 else f"{val:.{prec}f}"


def _markevery(n: int) -> int:
    return max(1, n // 8)


# ---------------------------------------------------------------------------
# CSV loading & config parsing
# ---------------------------------------------------------------------------

def load_csv(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path, skipinitialspace=True)
    return df.loc[:, ~df.columns.str.startswith("Unnamed")]


def parse_config(fname: str) -> tuple[str, int] | None:
    """Extract (label, cir_taps) from filename.
    """
    # latest format first (no _inj segment)
    m = re.match(r"sr([A-Za-z0-9._-]+?)_cir(\d+)", Path(fname).stem)
    if m:
        return m.group(1), int(m.group(2))
    # Fall back
    m = re.match(r"sr([A-Za-z0-9._-]+?)_inj[\d.]+us(?:_cir(\d+))?", Path(fname).stem)
    if m:
        label = m.group(1)
        cir = int(m.group(2)) if m.group(2) else 1
        return label, cir
    return None


def build_grouped_data(
    csv_files: list[Path],
) -> dict[int, dict[str, pd.DataFrame]]:
    """Return {cir: {label: df}}, keeping the first file per (label, cir) pair."""
    seen: dict[tuple[str, int], pd.DataFrame] = {}
    for p in csv_files:
        cfg = parse_config(p.name)
        if cfg is None:
            continue
        label, cir = cfg
        if (label, cir) in seen:
            continue
        df = load_csv(p)
        if not df.empty:
            seen[(label, cir)] = df

    by_cir: dict[int, dict[str, pd.DataFrame]] = {}
    for (label, cir), df in seen.items():
        by_cir.setdefault(cir, {})[label] = df
    return by_cir


# ---------------------------------------------------------------------------
# Comparison plots (one line per sample rate)
# ---------------------------------------------------------------------------

def _plot_sr(ax, i: int, sr: str, nodes, y, me: int, label: str | None = None):
    """Plot one sample-rate series with consistent color, filled marker, and shape."""
    ax.plot(nodes, y, color=_c(i), marker=_m(i),
            markerfacecolor=_c(i), markeredgecolor=_c(i),
            label=label or sr, markevery=me)


def _line_plot(sr_map: dict[str, pd.DataFrame], metric: str,
               ylabel: str, out_dir: Path, name: str, fmts: list[str],
               ylim_bottom: float | None = 0):
    fig, ax = plt.subplots()
    for i, (sr, df) in enumerate(sorted(sr_map.items())):
        df = _filter_saturated(df)
        if metric not in df.columns or df.empty:
            continue
        nodes = _link_count(df)
        _plot_sr(ax, i, sr, nodes, df[metric], _markevery(len(df)))
    ax.set_xlabel("Emulated links ($L = N^2$)")
    ax.set_ylabel(ylabel)
    ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    if ylim_bottom is not None:
        ax.set_ylim(bottom=ylim_bottom)
    ax.legend(framealpha=0.9)
    fig.tight_layout()
    _save(fig, out_dir, name, fmts)


def plot_throughput(sr_map, out_dir, suffix, fmts):
    _line_plot(sr_map, "throughput_signals_per_sec",
               "Throughput (signals/s)", out_dir,
               f"throughput{suffix}", fmts, ylim_bottom=0)


def plot_drop_rate(sr_map, out_dir, suffix, fmts):
    base_w, base_h = map(float, plt.rcParams["figure.figsize"])
    legend_w = LEGEND_COL_WIDTH_SINGLE
    fig = plt.figure(figsize=(base_w + legend_w, base_h))
    gs = fig.add_gridspec(1, 2, width_ratios=[base_w, legend_w],
                          wspace=LEGEND_COL_WSPACE)
    ax = fig.add_subplot(gs[0, 0])
    for i, (sr, df) in enumerate(sorted(sr_map.items())):
        df = _filter_saturated(df)
        if "drop_rate_pct" not in df.columns or df.empty:
            continue
        nodes = _link_count(df)
        _plot_sr(ax, i, sr, nodes, df["drop_rate_pct"], _markevery(len(df)))
    ax.axhline(10, color="gray", linestyle=":", linewidth=1.0, label="10% threshold")
    ax.set_xlabel("Emulated links ($L = N^2$)")
    ax.set_ylabel("Drop rate (\\%)")
    ax.set_yscale("symlog", linthresh=0.1)
    ax.yaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.set_ylim(bottom=0)
    ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    handles, labels = ax.get_legend_handles_labels()
    leg_ax = fig.add_subplot(gs[0, 1])
    leg_ax.axis("off")
    leg_ax.legend(handles, labels, title="Sample rate", framealpha=0.9,
                  loc="lower left", borderaxespad=0.0,
                  fontsize=LEGEND_FONT_SMALL,
                  title_fontsize=LEGEND_TITLE_FONT_SMALL,
                  handlelength=2.2, labelspacing=0.25, borderpad=0.25)
    fig.tight_layout()
    _save(fig, out_dir, f"drop_rate{suffix}", fmts)


def plot_e2e_latency(sr_map, out_dir, suffix, fmts):
    """p50 line with p99 shaded band per sample rate."""
    sample_df = next(iter(sr_map.values()))
    pct = _has_percentiles(sample_df)

    fig, ax = plt.subplots()
    for i, (sr, df) in enumerate(sorted(sr_map.items())):
        df = _filter_saturated(df, LATENCY_DROP_PCT)
        if df.empty:
            continue
        nodes = _link_count(df)
        me = _markevery(len(df))
        if pct:
            _plot_sr(ax, i, sr, nodes, df["latency_total_p50_us"], me)
            ax.fill_between(nodes, df["latency_total_p50_us"],
                            df["latency_total_p99_us"],
                            color=_c(i), alpha=0.12)
        else:
            _plot_sr(ax, i, sr, nodes, df["latency_total_avg_us"], me)
            ax.fill_between(nodes, df["latency_total_min_us"],
                            df["latency_total_max_us"],
                            color=_c(i), alpha=0.12)
    ax.set_xlabel("Emulated links ($L = N^2$)")
    ax.set_ylabel("Latency ($\\mu$s)")
    ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    ax.set_ylim(bottom=0)
    ax.legend(framealpha=0.9)
    fig.tight_layout()
    _save(fig, out_dir, f"e2e_latency{suffix}", fmts)


def plot_proc_latency(sr_map, out_dir, suffix, fmts):
    """Processing-only latency (excludes queue wait and delivery)."""
    sample_df = next(iter(sr_map.values()))
    pct = _has_percentiles(sample_df)

    base_w, base_h = map(float, plt.rcParams["figure.figsize"])
    legend_w = LEGEND_COL_WIDTH_SINGLE
    fig = plt.figure(figsize=(base_w + legend_w, base_h))
    gs = fig.add_gridspec(1, 2, width_ratios=[base_w, legend_w],
                          wspace=LEGEND_COL_WSPACE)
    ax = fig.add_subplot(gs[0, 0])
    for i, (sr, df) in enumerate(sorted(sr_map.items())):
        df = _filter_saturated(df, LATENCY_DROP_PCT)
        if df.empty:
            continue
        nodes = _link_count(df)
        me = _markevery(len(df))
        if pct and "latency_proc_p50_us" in df.columns:
            _plot_sr(ax, i, sr, nodes, df["latency_proc_p50_us"], me)
        elif "latency_proc_avg_us" in df.columns:
            _plot_sr(ax, i, sr, nodes, df["latency_proc_avg_us"], me)
    ax.set_xlabel("Emulated links ($L = N^2$)")
    ax.set_ylabel("Processing latency ($\\mu$s)")
    ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.yaxis.set_minor_formatter(ticker.NullFormatter())
    ax.set_ylim(bottom=70)
    handles, labels = ax.get_legend_handles_labels()
    leg_ax = fig.add_subplot(gs[0, 1])
    leg_ax.axis("off")
    leg_ax.legend(handles, labels, title="Sample rate", framealpha=0.9,
                  loc="lower left", borderaxespad=0.0,
                  fontsize=LEGEND_FONT_SMALL,
                  title_fontsize=LEGEND_TITLE_FONT_SMALL,
                  handlelength=2.2, labelspacing=0.25, borderpad=0.25)
    fig.tight_layout()
    _save(fig, out_dir, f"proc_latency{suffix}", fmts)


def plot_proc_latency_all_cir(by_cir: dict[int, dict[str, pd.DataFrame]],
                              out_dir: Path, fmts: list[str]):
    """Combined processing latency across CIR values.

    Encoding:
      - color/marker: sample rate
      - line style: CIR taps
    """
    if not by_cir:
        return

    all_srs = sorted({sr for sr_map in by_cir.values() for sr in sr_map.keys()})
    if not all_srs:
        return

    base_w, base_h = map(float, plt.rcParams["figure.figsize"])
    legend_w = LEGEND_COL_WIDTH_COMBINED
    fig = plt.figure(figsize=(base_w + legend_w, base_h))
    gs = fig.add_gridspec(1, 2, width_ratios=[base_w, legend_w],
                          wspace=LEGEND_COL_WSPACE)
    ax = fig.add_subplot(gs[0, 0])

    sr_idx = {sr: i for i, sr in enumerate(all_srs)}
    cirs = sorted(by_cir.keys())
    plotted_any = False

    # Distinct CIR styles: solid, long-dash, dotted with increasing width.
    cir_styles = [
        ("-",          1.4),   # CIR=1:  solid, thin
        ((0, (6, 3)),  1.8),   # CIR=10: long dash, medium
        ((0, (1.5, 2)), 2.2),  # CIR=20: dotted, thick
    ]

    for ci, cir in enumerate(cirs):
        sr_map = by_cir[cir]
        ls, lw = cir_styles[ci] if ci < len(cir_styles) else ("-", 1.4)
        for sr in all_srs:
            df = sr_map.get(sr)
            if df is None:
                continue
            df = _filter_saturated(df, LATENCY_DROP_PCT)
            if df.empty:
                continue

            col = None
            if _has_percentiles(df) and "latency_proc_p50_us" in df.columns:
                col = "latency_proc_p50_us"
            elif "latency_proc_avg_us" in df.columns:
                col = "latency_proc_avg_us"
            if col is None:
                continue

            idx = sr_idx[sr]
            nodes = df["tx_count"]
            ax.plot(nodes, df[col], color=_c(idx), marker=_m(idx),
                    markerfacecolor=_c(idx), markeredgecolor=_c(idx),
                    linestyle=ls, linewidth=lw,
                    markevery=_markevery(len(df)))
            plotted_any = True

    if not plotted_any:
        plt.close(fig)
        return

    ax.set_xlabel("Nodes ($N$)")
    ax.set_ylabel("Processing latency ($\\mu$s)")
    ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.yaxis.set_minor_formatter(ticker.NullFormatter())
    ax.set_ylim(bottom=70)

    # Combined legend: CIR section on top, sample-rate section bottom
    handles, labels = [], []
    # CIR section header
    handles.append(Line2D([], [], color="none", label=""))
    labels.append(r"$\bf{CIR\ taps}$")
    for ci, cir in enumerate(cirs):
        h = Line2D([0], [0], color="#444444",
                   linestyle=cir_styles[ci][0] if ci < len(cir_styles) else "-",
                   linewidth=cir_styles[ci][1] if ci < len(cir_styles) else 1.4)
        handles.append(h)
        labels.append(f"CIR={cir}")
    # Sample-rate section 
    handles.append(Line2D([], [], color="none", label=""))
    labels.append(r"$\bf{Sample\ rate}$")
    for sr in all_srs:
        idx = sr_idx[sr]
        h = Line2D([0], [0], color=_c(idx), marker=_m(idx),
                   markerfacecolor=_c(idx), markeredgecolor=_c(idx),
                   linestyle="-")
        handles.append(h)
        labels.append(sr)

    leg_ax = fig.add_subplot(gs[0, 1])
    leg_ax.axis("off")
    leg_ax.legend(handles, labels, framealpha=0.9,
                  loc="lower left", borderaxespad=0.0,
                  fontsize=LEGEND_FONT_SMALL,
                  handlelength=2.2, labelspacing=0.25, borderpad=0.25)

    fig.tight_layout()
    _save(fig, out_dir, "proc_latency_all_cir", fmts)


def plot_drop_rate_all_cir(by_cir: dict[int, dict[str, pd.DataFrame]],
                           out_dir: Path, fmts: list[str]):
    """Combined drop-rate plot across CIR values.

    Encoding:
      - color/marker: sample rate
      - line style: CIR taps
    """
    if not by_cir:
        return

    all_srs = sorted({sr for sr_map in by_cir.values() for sr in sr_map.keys()})
    if not all_srs:
        return

    base_w, base_h = map(float, plt.rcParams["figure.figsize"])
    legend_w = LEGEND_COL_WIDTH_COMBINED
    fig = plt.figure(figsize=(base_w + legend_w, base_h))
    gs = fig.add_gridspec(1, 2, width_ratios=[base_w, legend_w],
                          wspace=LEGEND_COL_WSPACE)
    ax = fig.add_subplot(gs[0, 0])

    sr_idx = {sr: i for i, sr in enumerate(all_srs)}
    cirs = sorted(by_cir.keys())
    plotted_any = False

    cir_styles = [
        ("-",          1.4),
        ((0, (6, 3)),  1.8),
        ((0, (1.5, 2)), 2.2),
    ]

    for ci, cir in enumerate(cirs):
        sr_map = by_cir[cir]
        ls, lw = cir_styles[ci] if ci < len(cir_styles) else ("-", 1.4)
        for sr in all_srs:
            df = sr_map.get(sr)
            if df is None or "drop_rate_pct" not in df.columns:
                continue
            df = _filter_saturated(df)
            if df.empty:
                continue

            idx = sr_idx[sr]
            nodes = _link_count(df)
            ax.plot(nodes, df["drop_rate_pct"], color=_c(idx), marker=_m(idx),
                    markerfacecolor=_c(idx), markeredgecolor=_c(idx),
                    linestyle=ls, linewidth=lw,
                    markevery=_markevery(len(df)))
            plotted_any = True

    if not plotted_any:
        plt.close(fig)
        return

    ax.axhline(10, color="gray", linestyle=":", linewidth=1.0)
    ax.set_xlabel("Emulated links ($L = N^2$)")
    ax.set_ylabel("Drop rate (\\%)")
    ax.set_yscale("symlog", linthresh=0.1)
    ax.yaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.set_ylim(bottom=0)
    ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))

    # Combined legend: CIR section on top, sample-rate section bottom
    handles, labels = [], []
    # CIR / ref section header
    handles.append(Line2D([], [], color="none"))
    labels.append(r"$\bf{CIR\ taps\ /\ ref}$")
    for ci, cir in enumerate(cirs):
        h = Line2D([0], [0], color="#444444",
                   linestyle=cir_styles[ci][0] if ci < len(cir_styles) else "-",
                   linewidth=cir_styles[ci][1] if ci < len(cir_styles) else 1.4)
        handles.append(h)
        labels.append(f"CIR={cir}")
    handles.append(Line2D([0], [0], color="gray", linestyle=":", linewidth=1.0))
    labels.append("10% threshold")
    # Sample-rate section header
    handles.append(Line2D([], [], color="none"))
    labels.append(r"$\bf{Sample\ rate}$")
    for sr in all_srs:
        idx = sr_idx[sr]
        h = Line2D([0], [0], color=_c(idx), marker=_m(idx),
                   markerfacecolor=_c(idx), markeredgecolor=_c(idx),
                   linestyle="-")
        handles.append(h)
        labels.append(sr)

    leg_ax = fig.add_subplot(gs[0, 1])
    leg_ax.axis("off")
    leg_ax.legend(handles, labels, framealpha=0.9,
                  loc="lower left", borderaxespad=0.0,
                  fontsize=LEGEND_FONT_SMALL,
                  handlelength=2.2, labelspacing=0.25, borderpad=0.25)

    fig.tight_layout()
    _save(fig, out_dir, "drop_rate_all_cir", fmts)


def plot_latency_breakdown(sr_map, out_dir, suffix, fmts):
    """Stacked-area latency breakdown for one representative sample rate."""
    # Use the lowest sample rate
    sr, df = min(sr_map.items(), key=lambda x: x[0])
    df = _filter_saturated(df, LATENCY_DROP_PCT)
    if df.empty:
        return
    pct = _has_percentiles(df)
    stat = "_p50_us" if pct else "_avg_us"
    components = [
        (f"latency_recv_proc{stat}", "Queue wait"),
        (f"latency_proc{stat}", "Processing"),
        (f"latency_proc_send{stat}", "Delivery"),
    ]
    available = [(c, l) for c, l in components if c in df.columns]
    if not available:
        return

    fig, ax = plt.subplots()
    nodes = _link_count(df)
    ys = np.array([df[c].values for c, _ in available])
    ax.stackplot(nodes, ys, labels=[l for _, l in available],
                 colors=COLORS[:len(available)], alpha=0.8)
    ax.set_xlabel("Emulated links ($L = N^2$)")
    stat_label = "p50" if pct else "avg"
    ax.set_ylabel(f"Latency, {stat_label} ($\\mu$s)")
    ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    ax.set_ylim(bottom=0)
    ax.legend(loc="upper left", framealpha=0.9)
    fig.tight_layout()
    _save(fig, out_dir, f"latency_breakdown{suffix}", fmts)


def plot_channel_steps(sr_map, out_dir, suffix, fmts):
    """Per-step impairment timing for the lowest sample rate."""
    sr, df = min(sr_map.items(), key=lambda x: x[0])
    df = _filter_saturated(df, LATENCY_DROP_PCT)
    if df.empty or not _has_detailed(df):
        return
    pct = _has_percentiles(df)
    stat = "_p50_us" if pct else "_avg_us"
    steps = [
        (f"latency_resample{stat}", "Resample"),
        (f"latency_cir{stat}", "CIR"),
        (f"latency_pathloss{stat}", "Path loss"),
        (f"latency_noise{stat}", "AWGN"),
        (f"latency_freq_offset{stat}", "Freq. offset"),
    ]
    fig, ax = plt.subplots()
    nodes = _link_count(df)
    me = _markevery(len(df))
    for i, (col, label) in enumerate(steps):
        if col in df.columns:
            ax.plot(nodes, df[col], color=_c(i), marker=_m(i),
                    markerfacecolor=_c(i), markeredgecolor=_c(i),
                    label=label, markevery=me)
    ax.set_xlabel("Emulated links ($L = N^2$)")
    ax.set_ylabel("Step latency ($\\mu$s)")
    ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    ax.set_ylim(bottom=0)
    ax.legend(framealpha=0.9, ncol=2)
    fig.tight_layout()
    _save(fig, out_dir, f"channel_steps{suffix}", fmts)


def plot_cdf(sr_map, out_dir, suffix, fmts):
    """CDF at selected link counts for the lowest sample rate."""
    sr, df = min(sr_map.items(), key=lambda x: x[0])
    df = _filter_saturated(df, LATENCY_DROP_PCT)
    if df.empty or not _has_percentiles(df):
        return

    fig, ax = plt.subplots()
    indices = _select_indices(len(df))
    for ci, idx in enumerate(indices):
        row = df.iloc[idx]
        n = int(row["tx_count"] * row["rx_count"])
        xs = [row["latency_total_min_us"], row["latency_total_p50_us"],
              row["latency_total_p90_us"], row["latency_total_p95_us"],
              row["latency_total_p99_us"], row["latency_total_p999_us"],
              row["latency_total_max_us"]]
        ys = [0.0, 0.50, 0.90, 0.95, 0.99, 0.999, 1.0]
        ax.plot(xs, ys, color=_c(ci), marker=_m(ci),
                label=f"$L={n}$", markersize=3.5)
    ax.set_xlabel("End-to-end latency ($\\mu$s)")
    ax.set_ylabel("CDF")
    ax.set_ylim(-0.03, 1.03)
    ax.legend(framealpha=0.9)
    fig.tight_layout()
    _save(fig, out_dir, f"latency_cdf{suffix}", fmts)


def plot_percentile_bars(sr_map, out_dir, suffix, fmts):
    """Grouped bars: p50/p95/p99/p99.9 at selected link counts per sample rate."""
    sample_df = next(iter(sr_map.values()))
    if not _has_percentiles(sample_df):
        return

    pcts = [
        ("latency_total_p50_us", "p50"),
        ("latency_total_p95_us", "p95"),
        ("latency_total_p99_us", "p99"),
        ("latency_total_p999_us", "p99.9"),
    ]
    srs = sorted(sr_map.keys())
    n_bars = len(pcts)
    bar_w = 0.8 / n_bars

    fig, axes = plt.subplots(1, len(srs),
                              figsize=(3.3 * len(srs), 2.8),
                              sharey=True, squeeze=False)
    axes = axes[0]

    for ax_i, sr in enumerate(srs):
        ax = axes[ax_i]
        df = _filter_saturated(sr_map[sr], LATENCY_DROP_PCT)
        if df.empty:
            ax.set_xlabel(sr)
            continue
        nodes = _link_count(df)
        indices = _select_indices(len(df))
        sel_nodes = [int(df.iloc[i]["tx_count"] * df.iloc[i]["rx_count"])
                     for i in indices]
        n_groups = len(sel_nodes)
        x = np.arange(n_groups)
        for pi, (col, label) in enumerate(pcts):
            vals = [df.iloc[i][col] for i in indices]
            offset = (pi - n_bars / 2 + 0.5) * bar_w
            ax.bar(x + offset, vals, bar_w, color=_c(pi), label=label)
        ax.set_xticks(x)
        ax.set_xticklabels([f"$L\\!=\\!{n}$" for n in sel_nodes], fontsize=7)
        ax.set_xlabel(sr)
        if ax_i == 0:
            ax.set_ylabel("Latency ($\\mu$s)")

    axes[0].legend(framealpha=0.9, fontsize=7, ncol=2)
    fig.tight_layout()
    _save(fig, out_dir, f"latency_percentiles{suffix}", fmts)


def plot_resources(sr_map, out_dir, suffix, fmts):
    """CPU time and memory comparison across sample rates."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7, 2.6))
    for i, (sr, df) in enumerate(sorted(sr_map.items())):
        df = _filter_saturated(df)
        if df.empty:
            continue
        nodes = _link_count(df)
        me = _markevery(len(df))
        _plot_sr(ax1, i, sr, nodes, df["cpu_user_ms"], me)
    ax1.set_xlabel("Emulated links ($L = N^2$)")
    ax1.set_ylabel("CPU user time (ms)")
    ax1.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    ax1.legend(framealpha=0.9)

    for i, (sr, df) in enumerate(sorted(sr_map.items())):
        df = _filter_saturated(df)
        if df.empty:
            continue
        nodes = _link_count(df)
        me = _markevery(len(df))
        mem_mb = df["peak_memory_kb"] / 1024.0
        _plot_sr(ax2, i, sr, nodes, mem_mb, me)
    ax2.set_xlabel("Emulated links ($L = N^2$)")
    ax2.set_ylabel("Peak RSS (MB)")
    ax2.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))

    fig.tight_layout()
    _save(fig, out_dir, f"resources{suffix}", fmts)


# ---------------------------------------------------------------------------
# LaTeX comparison table
# ---------------------------------------------------------------------------

def generate_latex_table(sr_map: dict[str, pd.DataFrame],
                         out_path: Path, cir: int):
    """Generate one or more readable tables comparing sample rates.

    To avoid overly wide, cramped tables, sample rates are chunked into
    smaller groups (default: 2 sample rates per table) while keeping the
    same selected channel-count columns.
    """
    ref_df = next(iter(sr_map.values()))
    pct = _has_percentiles(ref_df)
    detailed = _has_detailed(ref_df)
    srs = sorted(sr_map.keys())
    # Use one representative channel count controlled by TABLE_CHANNEL_COUNT.
    available_channels = sorted({int(v) for v in _link_count(ref_df).tolist()})
    if TABLE_CHANNEL_COUNT in available_channels:
        sel_nodes = [TABLE_CHANNEL_COUNT]
    else:
        fallback_c = available_channels[-1]
        print(f"    Warning: C={TABLE_CHANNEL_COUNT} not available for CIR={cir}; "
              f"using C={fallback_c} in LaTeX table.")
        sel_nodes = [fallback_c]

    # With one link-count column, all sample rates typically fit in one table.
    max_sr_per_table = len(srs) if len(sel_nodes) == 1 else 2
    sr_chunks = [srs[i:i + max_sr_per_table]
                 for i in range(0, len(srs), max_sr_per_table)]

    lines = []

    def _val(df, idx, col_prefix):
        row = df.iloc[idx]
        avg_col = f"{col_prefix}_avg_us"
        if pct:
            p99_col = f"{col_prefix}_p99_us"
            if avg_col in row and row.get(avg_col, 0) != 0:
                return f"{_fmt(row[avg_col])} ({_fmt(row[p99_col])})"
        if avg_col in row and row[avg_col] != 0:
            return _fmt(row[avg_col])
        return "--"

    for part_idx, srs_chunk in enumerate(sr_chunks, start=1):
        n_data_cols = len(srs_chunk) * len(sel_nodes)
        col_spec = "l" + "r" * n_data_cols
        part_suffix = (f" (part {part_idx}/{len(sr_chunks)})"
                       if len(sr_chunks) > 1 else "")

        lines.append("\\begin{table*}[t]")
        lines.append("\\centering")
        lines.append("\\small")
        if len(sel_nodes) == 1:
            lines.append(f"\\caption{{Latency breakdown ($\\mu$s) by sample rate at "
                         f"$C\\!=\\!{sel_nodes[0]}$ (CIR taps$={cir}$){part_suffix}.}}")
        else:
            lines.append(f"\\caption{{Latency breakdown ($\\mu$s) by sample rate and "
                         f"channel count (CIR taps$={cir}$){part_suffix}.}}")
        if part_idx == 1:
            lines.append(f"\\label{{tab:latency_cir{cir}}}")
        else:
            lines.append(f"\\label{{tab:latency_cir{cir}_part{part_idx}}}")
        lines.append(f"\\begin{{tabular}}{{{col_spec}}}")
        lines.append("\\toprule")

        header1 = ""
        for sr in srs_chunk:
            header1 += f" & \\multicolumn{{{len(sel_nodes)}}}{{c}}{{{sr}}}"
        header1 += " \\\\"
        lines.append(header1)

        rules = []
        col = 2
        for _ in srs_chunk:
            rules.append(f"\\cmidrule(lr){{{col}-{col + len(sel_nodes) - 1}}}")
            col += len(sel_nodes)
        lines.append(" ".join(rules))

        header2 = "Metric"
        for _ in srs_chunk:
            for nc in sel_nodes:
                header2 += f" & $C\\!=\\!{nc}$"
        header2 += " \\\\"
        lines.append(header2)
        lines.append("\\midrule")

        if pct:
            lines.append(f"\\multicolumn{{{n_data_cols + 1}}}{{l}}"
                         "{\\footnotesize Format: avg (p99)} \\\\")
            lines.append("\\midrule")

        def add_row(label, col_prefix, indent=False):
            pfx = "\\quad " if indent else ""
            row = f"{pfx}{label}"
            for sr in srs_chunk:
                df = sr_map[sr]
                nodes = _link_count(df)
                for nc in sel_nodes:
                    match = df.loc[nodes == nc]
                    if match.empty:
                        row += " & --"
                    else:
                        row += f" & {_val(match, 0, col_prefix)}"
            row += " \\\\"
            lines.append(row)

        add_row("\\textbf{End-to-end}", "latency_total")
        add_row("Queue wait", "latency_recv_proc", indent=True)
        add_row("Processing", "latency_proc", indent=True)
        add_row("Delivery", "latency_proc_send", indent=True)

        if detailed:
            lines.append("\\midrule")
            add_row("Resample", "latency_resample", indent=True)
            add_row("CIR", "latency_cir", indent=True)
            add_row("Path loss", "latency_pathloss", indent=True)
            add_row("AWGN", "latency_noise", indent=True)
            add_row("Freq. offset", "latency_freq_offset", indent=True)

        lines.append("\\midrule")

        row_tp = "Throughput (sig/s)"
        for sr in srs_chunk:
            df = sr_map[sr]
            nodes = _link_count(df)
            for nc in sel_nodes:
                match = df.loc[nodes == nc]
                if match.empty:
                    row_tp += " & --"
                else:
                    row_tp += f" & {match['throughput_signals_per_sec'].values[0]:.0f}"
        row_tp += " \\\\"
        lines.append(row_tp)

        row_dr = "Drop rate (\\%)"
        for sr in srs_chunk:
            df = sr_map[sr]
            nodes = _link_count(df)
            for nc in sel_nodes:
                match = df.loc[nodes == nc]
                if match.empty:
                    row_dr += " & --"
                else:
                    row_dr += f" & {_fmt(match['drop_rate_pct'].values[0])}"
        row_dr += " \\\\"
        lines.append(row_dr)

        lines.append("\\bottomrule")
        lines.append("\\end{tabular}")
        lines.append("\\end{table*}")
        lines.append("")

    out_path.write_text("\n".join(lines).rstrip() + "\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def generate_all(sr_map: dict[str, pd.DataFrame],
                 cir: int, out_dir: Path, fmts: list[str]):
    """Generate all comparison figures and one table for a CIR group."""
    if not sr_map:
        return
    suffix = f"_cir{cir}"
    n_sr = len(sr_map)

    print(f"  CIR={cir}: {n_sr} sample rate(s)")

    plot_throughput(sr_map, out_dir, suffix, fmts)
    plot_drop_rate(sr_map, out_dir, suffix, fmts)
    plot_e2e_latency(sr_map, out_dir, suffix, fmts)
    plot_proc_latency(sr_map, out_dir, suffix, fmts)
    plot_latency_breakdown(sr_map, out_dir, suffix, fmts)
    plot_channel_steps(sr_map, out_dir, suffix, fmts)
    plot_cdf(sr_map, out_dir, suffix, fmts)
    plot_percentile_bars(sr_map, out_dir, suffix, fmts)
    plot_resources(sr_map, out_dir, suffix, fmts)

    tex_path = out_dir / f"latency_table{suffix}.tex"
    generate_latex_table(sr_map, tex_path, cir)
    print(f"    Table: {tex_path.name}")


def main():
    parser = argparse.ArgumentParser(
        description="Post-process CHEM benchmark suite results into "
                    "publication-ready comparison plots and LaTeX tables.")
    parser.add_argument("input", type=str,
                        help="Benchmark results directory")
    parser.add_argument("-o", "--output", type=str, default=None,
                        help="Output directory (default: <input>/figures/)")
    parser.add_argument("--formats", nargs="+", default=["pdf", "png"],
                        choices=["pdf", "png", "eps", "svg"],
                        help="Output figure formats (default: pdf png)")
    args = parser.parse_args()

    input_path = Path(args.input)
    if not input_path.exists():
        print(f"Error: {input_path} not found")
        sys.exit(1)

    if input_path.is_dir():
        csv_files = sorted(input_path.glob("*.csv"))
    else:
        csv_files = [input_path]

    if not csv_files:
        print("Error: no CSV files found")
        sys.exit(1)

    out_dir = Path(args.output) if args.output else (
        input_path / "figures" if input_path.is_dir()
        else input_path.parent / f"{input_path.stem}_figures"
    )
    out_dir.mkdir(parents=True, exist_ok=True)
    fmts = args.formats

    print(f"Output:  {out_dir}")
    print(f"Formats: {', '.join(fmts)}")
    print(f"CSVs:    {len(csv_files)}\n")

    by_cir = build_grouped_data(csv_files)
    if not by_cir:
        print("Error: no parseable benchmark CSV files found")
        sys.exit(1)

    for cir in sorted(by_cir.keys()):
        generate_all(by_cir[cir], cir, out_dir, fmts)

    if len(by_cir) > 1:
        plot_drop_rate_all_cir(by_cir, out_dir, fmts)
        print("  Combined: drop_rate_all_cir")
        plot_proc_latency_all_cir(by_cir, out_dir, fmts)
        print("  Combined: proc_latency_all_cir")

    total_figs = sum(len(list(out_dir.glob(f"*.{f}"))) for f in fmts)
    print(f"\nDone. {total_figs} files in {out_dir}/")


if __name__ == "__main__":
    main()
