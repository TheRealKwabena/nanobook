#!/usr/bin/env python3
"""Score one day out-of-sample, then retrain. The nightly loop.

Order matters and is not negotiable:

    1. load the newest model whose training data ends STRICTLY BEFORE this day
    2. predict this day with it, and score against what actually happened
    3. append the result to the append-only scorecard
    4. only THEN retrain including this day, and write tomorrow's model

Doing step 4 before step 2 would make every number in-sample and every result
meaningless. Because the model file is written before the next day's data has even
been published by IEX, the predictions it makes are out-of-sample in the strong
sense: made before the outcome was knowable, not merely held out from a shuffle.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "python"))

from nanobook import model as M          # noqa: E402
from nanobook import scoring, store      # noqa: E402
from nanobook.features import FEATURE_COLUMNS, PanelConfig, panel_from_files  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", type=Path, default=REPO / "data")
    ap.add_argument("--date", default="all",
                    help="YYYYMMDD, or 'all' to walk every available day in order")
    ap.add_argument("--horizon-s", type=int, default=10)
    ap.add_argument("--alpha", type=float, default=50.0)
    ap.add_argument("--train-days", type=int, default=20,
                    help="rolling training window in days")
    ap.add_argument("--cost", type=float, default=0.5,
                    help="fraction of the quoted spread paid per position")
    ap.add_argument("--rescore", action="store_true",
                    help="re-score days already in the scorecard")
    args = ap.parse_args()

    layout = store.Layout(args.data)
    days = layout.available_days()
    if not days:
        print(f"no feature files in {layout.features} — run scripts/fetch_day.py first",
              file=sys.stderr)
        return 2

    targets = days if args.date == "all" else [args.date]
    cfg = PanelConfig(horizon_s=args.horizon_s)

    config = {"features": FEATURE_COLUMNS, "horizon_s": args.horizon_s,
              "alpha": args.alpha, "train_days": args.train_days,
              "cost_multiplier": args.cost, "model": "ridge"}
    cid = store.config_id(config)
    n_trials = store.register_trial(layout, config)

    print(f"config {cid}   ({n_trials} configuration"
          f"{'s' if n_trials != 1 else ''} tried to date)")
    print(f"horizon {args.horizon_s}s  alpha {args.alpha}  train window {args.train_days}d  "
          f"cost {args.cost}x spread\n")

    for day in targets:
        if day not in days:
            print(f"{day}: no feature file, skipping")
            continue
        if not args.rescore and store.already_scored(layout, day, cid):
            print(f"{day}: already scored for this config (use --rescore to redo)")
            continue

        prior = layout.model_trained_before(day)
        panel = panel_from_files([layout.feature_path(day)], cfg)

        if panel.empty:
            print(f"{day}: no usable rows after filtering, skipping")
            continue

        if prior is None:
            print(f"{day}: no model predates this day — training only, nothing to score")
        else:
            fitted = M.FittedModel.from_json(prior)
            pred = fitted.predict(panel)
            sc = scoring.score(panel, pred, horizon_s=args.horizon_s,
                               cost_multiplier=args.cost)
            dsr = scoring.deflated_sharpe(sc.sharpe_per_bar, n_trials, sc.n)

            record = {
                "date": day,
                "config_id": cid,
                "scored_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                "model_file": prior.name,
                "model_trained_through": fitted.trained_through,
                "model_train_rows": fitted.n_train_rows,
                "model_fingerprint": fitted.train_fingerprint,
                "n_symbols": int(panel["symbol"].nunique()),
                "deflated_sharpe": dsr,
                "n_trials": n_trials,
                **sc.as_dict(),
            }
            store.append_score(layout, record)

            print(f"{day}: model {prior.name} (trained through {fitted.trained_through}, "
                  f"{fitted.n_train_rows:,} rows)")
            print(f"        n={sc.n:,}  IC {sc.ic_spearman:+.4f}  hit {sc.hit_rate:.3f}  "
                  f"gross {sc.gross_bps:+.3f}bp (t={sc.gross_t:+.2f})  "
                  f"net {sc.net_bps:+.3f}bp (t={sc.net_t:+.2f})  DSR {dsr:.3f}")

        # --- retrain, now that scoring is done ---
        window = [d for d in days if d <= day][-args.train_days:]
        train = panel_from_files([layout.feature_path(d) for d in window], cfg)
        if train.empty:
            print(f"        cannot retrain: no usable training rows")
            continue
        fitted = M.fit(train, alpha=args.alpha)
        out = layout.model_path(day)
        fitted.to_json(out)
        print(f"        retrained on {len(window)} day(s), {fitted.n_train_rows:,} rows "
              f"-> {out.name}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
