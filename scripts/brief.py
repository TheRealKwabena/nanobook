#!/usr/bin/env python3
"""nanobook brief — what the books said yesterday, and how the signal is doing.

Prints three things, in descending order of how much they should change your mind:

  1. THE OUT-OF-SAMPLE SCORECARD. Every day scored by a model that predated it,
     pooled, with Newey-West t-statistics and a deflated Sharpe that accounts for
     how many configurations have been tried. This is the only part that is
     evidence of anything.
  2. YESTERDAY'S MARKET, reconstructed from the raw feed.
  3. THE CURRENT MODEL's coefficients, so the signal can be argued with.

The verdict line is written to be capable of saying "nothing here". A brief that
can only report success is a brief that is not measuring anything.
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "python"))

import numpy as np      # noqa: E402
import pandas as pd     # noqa: E402

from nanobook import model as M                                  # noqa: E402
from nanobook import scoring, store                              # noqa: E402
from nanobook.features import FEATURE_COLUMNS, PanelConfig, load_day, panel_from_files  # noqa: E402


class Style:
    def __init__(self, enabled: bool):
        self.on = enabled

    def _w(self, code: str, s: str) -> str:
        return f"\033[{code}m{s}\033[0m" if self.on else s

    def bold(self, s): return self._w("1", s)
    def dim(self, s): return self._w("2", s)
    def green(self, s): return self._w("32", s)
    def red(self, s): return self._w("31", s)
    def yellow(self, s): return self._w("33", s)
    def cyan(self, s): return self._w("36", s)

    def signed(self, v: float, good_positive: bool = True, fmt: str = "+.3f") -> str:
        if not math.isfinite(v):
            return self.dim("   n/a")
        s = f"{v:{fmt}}"
        if abs(v) < 1e-12:
            return s
        positive = v > 0
        return self.green(s) if positive == good_positive else self.red(s)


def rule(style: Style, title: str = "", width: int = 74) -> str:
    if not title:
        return style.dim("─" * width)
    pad = width - len(title) - 3
    return style.dim("── ") + style.bold(title) + " " + style.dim("─" * max(pad, 0))


def fmt_money(cents: float) -> str:
    d = cents / 100.0
    for unit, div in (("B", 1e9), ("M", 1e6), ("k", 1e3)):
        if abs(d) >= div:
            return f"${d/div:.1f}{unit}"
    return f"${d:.0f}"


def pooled_stats(records: list[dict], n_trials: int) -> dict:
    """Aggregate per-day scorecards into one pooled view, weighting by sample size."""
    if not records:
        return {}
    n = np.array([r["n"] for r in records], dtype=float)
    total = n.sum()

    def wmean(key: str) -> float:
        v = np.array([r.get(key, np.nan) for r in records], dtype=float)
        ok = np.isfinite(v)
        return float(np.sum(v[ok] * n[ok]) / np.sum(n[ok])) if ok.any() else float("nan")

    # Pooling t-statistics is not valid, so the daily net returns are treated as
    # the sample and a t-stat computed across days. With a handful of days this is
    # weak by construction, which is the honest position.
    daily_net = np.array([r["net_bps"] for r in records], dtype=float)
    daily_net = daily_net[np.isfinite(daily_net)]
    if len(daily_net) >= 2 and daily_net.std(ddof=1) > 0:
        t_across_days = daily_net.mean() / (daily_net.std(ddof=1) / math.sqrt(len(daily_net)))
    else:
        t_across_days = float("nan")

    sharpe_bar = wmean("sharpe_per_bar")
    return {
        "days": len(records),
        "rows": int(total),
        "ic": wmean("ic_spearman"),
        "hit": wmean("hit_rate"),
        "gross_bps": wmean("gross_bps"),
        "net_bps": wmean("net_bps"),
        "spread_bps": wmean("mean_spread_bps"),
        "tradeable": wmean("tradeable_fraction"),
        "reversal_ic": wmean("baseline_reversal_ic"),
        "moved": wmean("moved_fraction"),
        "sharpe_per_bar": sharpe_bar,
        "t_across_days": t_across_days,
        "dsr": scoring.deflated_sharpe(sharpe_bar, n_trials, int(total)),
        "worst_day_net": float(np.min(daily_net)) if len(daily_net) else float("nan"),
        "best_day_net": float(np.max(daily_net)) if len(daily_net) else float("nan"),
        "days_positive": int(np.sum(daily_net > 0)) if len(daily_net) else 0,
    }


def verdict(p: dict, style: Style) -> list[str]:
    """One honest paragraph. Willing to conclude that there is nothing here."""
    if not p:
        return [style.yellow("No scored days yet — run scripts/run_day.py.")]

    lines = []
    net, t, dsr = p["net_bps"], p["t_across_days"], p["dsr"]
    ic = p["ic"]

    if not math.isfinite(net):
        return [style.yellow("Not enough data to judge.")]

    if net <= 0:
        lines.append(style.red("No tradeable edge.") +
                     f" Net of {p['spread_bps']:.1f} bp of spread the signal loses "
                     f"{abs(net):.2f} bp per bar.")
        if math.isfinite(ic) and abs(ic) > 0.01:
            lines.append(style.dim(
                f"  The direction is informative (IC {ic:+.4f}) but the moves it "
                f"predicts are smaller than the cost of trading them."))
        lines.append(style.dim(
            f"  Only {p['tradeable']*100:.1f}% of predictions even exceed half the spread."))
    elif not math.isfinite(t) or abs(t) < 2.0:
        lines.append(style.yellow("Positive but not significant.") +
                     f" Net {net:+.2f} bp per bar, t={t:+.2f} across "
                     f"{p['days']} days — indistinguishable from noise.")
    elif math.isfinite(dsr) and dsr < 0.95:
        lines.append(style.yellow("Significant, but not after multiple-testing.") +
                     f" Net {net:+.2f} bp (t={t:+.2f}), deflated Sharpe {dsr:.3f} "
                     f"against {p.get('n_trials', 1)} configurations tried.")
    else:
        lines.append(style.green("Survives the checks so far.") +
                     f" Net {net:+.2f} bp per bar, t={t:+.2f}, deflated Sharpe {dsr:.3f}.")
        lines.append(style.dim("  Small-sample caveat still applies; keep accumulating days."))

    rev = p.get("reversal_ic", float("nan"))
    if math.isfinite(rev) and math.isfinite(ic) and rev > ic:
        lines.append(style.yellow("  The baseline beats the model.") + style.dim(
            f" Plain short-horizon reversal scores IC {rev:+.4f} against the "
            f"model's {ic:+.4f}, so the fitted coefficients are not adding "
            f"information over one line of code."))

    if p["days"] < 20:
        lines.append(style.dim(
            f"  {p['days']} scored day(s). Treat everything above as provisional until "
            f"there are at least 20."))
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", type=Path, default=REPO / "data")
    ap.add_argument("--no-color", action="store_true")
    ap.add_argument("--horizon-s", type=int, default=10)
    args = ap.parse_args()

    color = sys.stdout.isatty() and not args.no_color and not os.environ.get("NO_COLOR")
    st = Style(color)
    layout = store.Layout(args.data)

    days = layout.available_days()
    records = store.read_scores(layout)
    n_trials = 1
    if layout.trials.exists():
        import json
        try:
            n_trials = max(1, len(json.loads(layout.trials.read_text())))
        except json.JSONDecodeError:
            pass

    print()
    print(st.bold("  nanobook brief"), st.dim(f" · {len(days)} day(s) of IEX DEEP on disk"))
    print()

    # ---------------- 1. out-of-sample scorecard ----------------
    print(rule(st, "out-of-sample scorecard"))
    if not records:
        print("  " + st.yellow("nothing scored yet — run: python3 scripts/run_day.py"))
    else:
        print(st.dim(f"  {'date':<10} {'rows':>8} {'IC':>8} {'hit':>6} "
                     f"{'gross':>9} {'net':>9} {'t(net)':>8} {'spread':>8}"))
        for r in records[-12:]:
            print(f"  {r['date']:<10} {r['n']:>8,} "
                  f"{st.signed(r['ic_spearman'], True, '+.4f'):>8} "
                  f"{r['hit_rate']:>6.3f} "
                  f"{st.signed(r['gross_bps'], True):>9} "
                  f"{st.signed(r['net_bps'], True):>9} "
                  f"{st.signed(r['net_t'], True, '+.2f'):>8} "
                  f"{r['mean_spread_bps']:>7.2f}b")

        p = pooled_stats(records, n_trials)
        p["n_trials"] = n_trials
        print()
        print(f"  pooled over {p['days']} day(s), {p['rows']:,} bars, "
              f"{n_trials} configuration{'s' if n_trials != 1 else ''} tried")
        print(f"    information coefficient   {st.signed(p['ic'], True, '+.4f')}"
              f"     (reversal baseline {p['reversal_ic']:+.4f})")
        print(f"    gross / net per bar       {st.signed(p['gross_bps'])} bp  /  "
              f"{st.signed(p['net_bps'])} bp")
        print(f"    mean quoted spread        {p['spread_bps']:.2f} bp")
        print(f"    predictions beating cost  {p['tradeable']*100:.1f}%")
        print(f"    mid moved within horizon  {p.get('moved', float('nan'))*100:.1f}%"
              + st.dim("   (hit rate is conditioned on these)"))
        print(f"    t across days             {st.signed(p['t_across_days'], True, '+.2f')}"
              f"        ({p['days_positive']}/{p['days']} days net positive)")
        print(f"    deflated Sharpe           {p['dsr']:.3f}"
              + st.dim("   (needs > 0.95 to beat the best of the configurations tried)"))
        print()
        for line in verdict(p, st):
            print("  " + line)
    print()

    # ---------------- 2. yesterday's market ----------------
    if days:
        latest = days[-1]
        raw = load_day(layout.feature_path(latest))
        print(rule(st, f"most recent session · {latest[:4]}-{latest[4:6]}-{latest[6:]}"))
        if raw.empty:
            print("  " + st.yellow("no two-sided quotes in this file"))
        else:
            panel = panel_from_files([layout.feature_path(latest)],
                                     PanelConfig(horizon_s=args.horizon_s))
            print(st.dim(f"  {'symbol':<8} {'bars':>7} {'kept':>7} {'spread*':>8} "
                         f"{'trades':>8} {'volume':>10} {'notional':>10} {'flow':>7}"))
            kept_by_sym = panel.groupby("symbol").size().to_dict() if not panel.empty else {}
            kept_spread = (panel.groupby("symbol")["spread_bps"].median().to_dict()
                           if not panel.empty else {})
            for sym, g in raw.groupby("symbol"):
                buy = g["buy_shares"].sum()
                sell = g["sell_shares"].sum()
                flow = (buy - sell) / (buy + sell) if (buy + sell) > 0 else 0.0
                # Median spread over the TRADEABLE bars. The raw median is
                # dominated by near-empty-book quotes: MSFT's unfiltered median on
                # IEX is 240 bp, which is not a spread anyone could trade against.
                sp = kept_spread.get(sym, float("nan"))
                sp_txt = f"{sp:>7.2f}b" if math.isfinite(sp) else st.dim("      —")
                print(f"  {sym:<8} {len(g):>7,} {kept_by_sym.get(sym, 0):>7,} "
                      f"{sp_txt} {int(g['trades'].sum()):>8,} "
                      f"{int(g['trade_shares'].sum()):>10,} "
                      f"{fmt_money(float(g['notional_cents'].sum())):>10} "
                      f"{st.signed(flow, True, '+.3f'):>7}")
            print(st.dim("\n  'kept' is bars surviving the tradeability filters "
                         "(spread, staleness, round-lot depth);"))
            print(st.dim("  spread* is the median over those kept bars, not over all "
                         "quotes."))
    print()

    # ---------------- 3. the current model ----------------
    model_files = sorted(layout.models.glob("model_*.json")) if layout.models.exists() else []
    if model_files:
        fitted = M.FittedModel.from_json(model_files[-1])
        print(rule(st, "current model"))
        print(f"  ridge alpha {fitted.alpha:g}, trained through {fitted.trained_through} "
              f"on {fitted.n_train_rows:,} bars over {len(fitted.train_dates)} day(s)")
        print(st.dim(f"  fingerprint {fitted.train_fingerprint}   file {model_files[-1].name}"))
        print()
        coefs = np.asarray(fitted.coefficients) * fitted.target_std
        order = np.argsort(-np.abs(coefs))
        scale = float(np.max(np.abs(coefs))) or 1.0
        print(st.dim("  coefficients, in bp of forward return per standard deviation:"))
        for i in order[:6]:
            coef = float(coefs[i])
            bar = "█" * max(1, int(round(abs(coef) / scale * 26)))
            colored = st.green(bar) if coef > 0 else st.red(bar)
            print(f"    {fitted.feature_names[i]:<18} {coef:+7.3f}  {colored}")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
