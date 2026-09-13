#!/usr/bin/env python3
"""End-to-end check of the daily loop on synthetic data, for CI.

Builds several days of feature files containing a KNOWN relationship, runs the
real walk-forward loop over them, and asserts that the out-of-sample scorecard
recovers it with the right sign. Then does the same with pure noise and asserts
that it recovers nothing.

This exercises the actual scripts rather than the library functions, so a mistake
in the orchestration — scoring after retraining, say, or reusing a model that saw
the day it is scoring — is caught here even though every unit test passes.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "python"))
sys.path.insert(0, str(REPO / "tests"))

import numpy as np  # noqa: E402

from nanobook import store  # noqa: E402

SEC = 1_000_000_000


def build_day(path: Path, day_index: int, *, signal: bool, n: int = 6000) -> None:
    """One synthetic session.

    With `signal`, a persistent latent state tilts the book AND drives the mid over
    the FOLLOWING bars. The ordering is the whole point: an earlier version of this
    generator tilted the book and moved the mid on the same bar, which is
    contemporaneous rather than predictive. That version still appeared to
    "recover" a signal — but only through a spurious price-level channel in the
    features, so it would have passed while measuring nothing at all. The latent
    state must lead the return, never coincide with it.
    """
    rng = np.random.default_rng(1000 + day_index)
    from test_research import write_feature_csv  # reuse the CSV writer

    # 09:35 ET on a December day, in UTC nanoseconds.
    start = (1577457300 + day_index * 86400) * SEC

    # Persistent latent state, so a multi-bar horizon can actually capture it.
    latent = np.zeros(n)
    for i in range(1, n):
        latent[i] = 0.97 * latent[i - 1] + rng.normal()

    rows = []
    mid = 1_000_000
    for i in range(n):
        # The book tilts with the CURRENT latent state...
        tilt = float(np.tanh(latent[i] * 0.6))
        bsz = int(round(20 * (1 + tilt) + 3)) * 100
        asz = int(round(20 * (1 - tilt) + 3)) * 100

        # ...while the mid moves with the PREVIOUS state, so the book leads.
        drive = latent[i - 1] if (signal and i > 0) else 0.0
        step = 45.0 * drive + rng.normal(scale=150.0)
        mid = int(max(500_000, mid + step))
        mid -= mid % 100

        rows.append(dict(
            ts_ns=start + i * SEC, symbol="TEST",
            bid=mid - 50, bid_shares=bsz, ask=mid + 50, ask_shares=asz, spread=100,
            bid_depth=bsz * 3, ask_depth=asz * 3, bid_levels=5, ask_levels=5,
            trades=int(rng.integers(0, 3)), trade_shares=int(rng.integers(0, 400)),
            buy_shares=int(rng.integers(0, 200)), sell_shares=int(rng.integers(0, 200)),
            unclassified_shares=0, notional_cents=1000, last_price=mid,
            odd_lot_trades=0, sweep_trades=0, trading_status="T"))
    write_feature_csv(path, rows)


def run_loop(root: Path, *, signal: bool, days: int = 5) -> list[dict]:
    layout = store.Layout(root)
    for d in range(days):
        build_day(layout.feature_path(f"2019121{d}"), d, signal=signal)
    rc = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "run_day.py"),
         "--data", str(root), "--date", "all", "--horizon-s", "5", "--alpha", "10"],
        capture_output=True, text=True)
    if rc.returncode != 0:
        print(rc.stdout); print(rc.stderr, file=sys.stderr)
        raise SystemExit("run_day.py failed")
    return store.read_scores(layout)


def main() -> int:
    print("=== loop on data WITH a planted signal ===")
    with tempfile.TemporaryDirectory() as td:
        recs = run_loop(Path(td), signal=True)
        assert recs, "no days were scored"
        ics = [r["ic_spearman"] for r in recs]
        print(f"  scored {len(recs)} days, out-of-sample IC: "
              + ", ".join(f"{v:+.3f}" for v in ics))
        mean_ic = float(np.mean(ics))
        assert mean_ic > 0.05, f"failed to recover a planted signal (mean IC {mean_ic:+.4f})"
        # Every day must be scored by a model that predates it.
        for r in recs:
            assert r["model_trained_through"] < r["date"], (
                f"model {r['model_file']} was trained through "
                f"{r['model_trained_through']} but scored {r['date']}")
        print(f"  mean IC {mean_ic:+.4f}  — recovered, and every model predates its day")

    print("\n=== loop on PURE NOISE ===")
    with tempfile.TemporaryDirectory() as td:
        recs = run_loop(Path(td), signal=False)
        ics = [r["ic_spearman"] for r in recs]
        print(f"  scored {len(recs)} days, out-of-sample IC: "
              + ", ".join(f"{v:+.3f}" for v in ics))
        mean_ic = float(np.mean(ics))
        assert abs(mean_ic) < 0.05, f"found signal in noise (mean IC {mean_ic:+.4f})"
        print(f"  mean IC {mean_ic:+.4f}  — correctly found nothing")

    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
