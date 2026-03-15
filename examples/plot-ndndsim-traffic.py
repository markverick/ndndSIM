#!/usr/bin/env python3
"""
plot-ndndsim-traffic.py — Plot classified link traffic from ndndSIM.

Reads a link-traffic CSV produced by NdndLinkTracer (with per-category
columns) and shows traffic composition as stacked area charts.

Categories: DvAdvert, PrefixSync, Mgmt, UserInterest, UserData, Other

Usage examples:

  # Basic
  python3 plot-ndndsim-traffic.py link-traffic.csv

  # With events overlay
  python3 plot-ndndsim-traffic.py link-traffic.csv -e events.csv

  # Zoom into a time window
  python3 plot-ndndsim-traffic.py link-traffic.csv --tmin 10 --tmax 50

  # Custom title and output format
  python3 plot-ndndsim-traffic.py link-traffic.csv -o traffic.pdf \\
      --title "Sprint Topology Link Traffic"
"""

import argparse
import sys
from pathlib import Path

import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

# ── Category display config ─────────────────────────────────────────

CATEGORIES = [
    ("DvAdvert",     "DV Routing",      "#e74c3c"),  # red
    ("PrefixSync",   "Prefix Sync",     "#f39c12"),  # orange
    ("Mgmt",         "Management",      "#9b59b6"),  # purple
    ("UserInterest", "User Interest",   "#3498db"),  # blue
    ("UserData",     "User Data",       "#2ecc71"),  # green
    ("Other",        "Other",           "#95a5a6"),  # grey
]

# ── Event styles ────────────────────────────────────────────────────

EVENT_STYLES = {
    "LinkDown":  {"color": "red",    "ls": "--", "label": "Link Down"},
    "LinkUp":    {"color": "green",  "ls": "--", "label": "Link Up"},
    "PrefixAdd": {"color": "blue",   "ls": ":",  "label": "Prefix Add"},
    "PrefixDel": {"color": "orange", "ls": ":",  "label": "Prefix Del"},
}

_FALLBACK_COLORS = ["purple", "brown", "teal", "magenta", "olive", "cyan"]
_FALLBACK_IDX = 0


def _event_style(event_type: str) -> dict:
    global _FALLBACK_IDX
    if event_type in EVENT_STYLES:
        return EVENT_STYLES[event_type]
    color = _FALLBACK_COLORS[_FALLBACK_IDX % len(_FALLBACK_COLORS)]
    _FALLBACK_IDX += 1
    style = {"color": color, "ls": "-.", "label": event_type}
    EVENT_STYLES[event_type] = style
    return style


# ── Data loading ────────────────────────────────────────────────────

def load_link_csv(path: str, tmin: float | None,
                  tmax: float | None) -> pd.DataFrame:
    df = pd.read_csv(path)
    if tmin is not None:
        df = df[df["Time"] >= tmin]
    if tmax is not None:
        df = df[df["Time"] <= tmax]
    return df


def load_events(path: str, tmin: float | None,
                tmax: float | None) -> pd.DataFrame:
    df = pd.read_csv(path)
    if tmin is not None:
        df = df[df["Time"] >= tmin]
    if tmax is not None:
        df = df[df["Time"] <= tmax]
    return df


# ── Plotting ────────────────────────────────────────────────────────

def _draw_events(axes: list, events_df: pd.DataFrame) -> dict:
    legend_handles: dict[str, mpatches.Patch] = {}
    for _, row in events_df.iterrows():
        style = _event_style(row["Type"])
        for ax in axes:
            ax.axvline(row["Time"], color=style["color"],
                       linestyle=style["ls"], alpha=0.7, linewidth=0.9)
        if row["Type"] not in legend_handles:
            legend_handles[row["Type"]] = mpatches.Patch(
                color=style["color"], label=style["label"])
    return legend_handles


def plot_link_traffic(link_df: pd.DataFrame,
                     events_df: pd.DataFrame | None,
                     title: str, output: str,
                     exclude: set[str] | None = None):
    t = link_df["Time"]
    exclude = exclude or set()

    # Build per-category series for bytes (KB) and packets
    kb_series = {}
    pkt_series = {}
    for cat_key, _, _ in CATEGORIES:
        if cat_key in exclude:
            continue
        bcol = f"{cat_key}_Bytes"
        pcol = f"{cat_key}_Pkts"
        if bcol in link_df.columns:
            kb_series[cat_key] = link_df[bcol] / 1024.0
            pkt_series[cat_key] = link_df[pcol]

    # Filter to categories with non-zero data
    active = [(k, l, c) for k, l, c in CATEGORIES if k in kb_series]
    if not active:
        print("WARNING: no recognised traffic categories in CSV", file=sys.stderr)
        return

    fig, (ax_kb, ax_pkt) = plt.subplots(
        2, 1, figsize=(14, 8), sharex=True,
        layout="constrained",
        gridspec_kw={"hspace": 0.08})

    # ── Stacked KB/s ──
    kb_stack = [kb_series[k] for k, _, _ in active]
    colors = [c for _, _, c in active]
    labels = [l for _, l, _ in active]
    ax_kb.stackplot(t, *kb_stack, colors=colors, labels=labels, alpha=0.8)
    ax_kb.set_ylabel("Link Traffic (KB/s)")
    ax_kb.set_title(title)
    ax_kb.grid(True, alpha=0.3)
    ax_kb.legend(loc="upper right", fontsize=8, framealpha=0.8, ncol=3)

    # ── Stacked packets/s ──
    pkt_stack = [pkt_series[k] for k, _, _ in active]
    ax_pkt.stackplot(t, *pkt_stack, colors=colors, labels=labels, alpha=0.8)
    ax_pkt.set_ylabel("Packets/s")
    ax_pkt.set_xlabel("Simulation Time (s)")
    ax_pkt.grid(True, alpha=0.3)

    # ── Events overlay ──
    if events_df is not None and not events_df.empty:
        event_handles = _draw_events([ax_kb, ax_pkt], events_df)
        if event_handles:
            # Add event legend to lower axis
            ax_pkt.legend(handles=list(event_handles.values()),
                         loc="upper right", fontsize=8, framealpha=0.8, ncol=2)

    fig.savefig(output, dpi=150)
    print(f"Saved plot to {output}")
    plt.close(fig)


# ── CLI ─────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)

    p.add_argument("link", metavar="LINK_CSV",
                   help="Path to link-traffic CSV from NdndLinkTracer")
    p.add_argument("-e", "--events", default=None,
                   help="Path to events CSV (optional)")
    p.add_argument("-o", "--output", default="results/ndndsim-traffic.png",
                   help="Output image file (default: %(default)s)")
    p.add_argument("--title", default=None,
                   help="Plot title")
    p.add_argument("--tmin", type=float, default=None,
                   help="Start of time window (seconds)")
    p.add_argument("--tmax", type=float, default=None,
                   help="End of time window (seconds)")
    p.add_argument("--exclude", nargs="+", default=[],
                   metavar="CAT",
                   help="Category keys to exclude (e.g. UserInterest UserData Other)")

    args = p.parse_args()

    for path_arg in [args.link, args.events]:
        if path_arg and not Path(path_arg).exists():
            print(f"ERROR: {path_arg} not found.", file=sys.stderr)
            sys.exit(1)

    link_df = load_link_csv(args.link, args.tmin, args.tmax)
    events_df = (load_events(args.events, args.tmin, args.tmax)
                 if args.events else None)

    title = args.title or "ndndSIM Link Traffic Composition"
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    plot_link_traffic(link_df, events_df, title, args.output,
                      exclude=set(args.exclude))


if __name__ == "__main__":
    main()
