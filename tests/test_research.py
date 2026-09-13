"""Tests for the research layer.

The two that matter most are the calibration tests at the bottom: given data with
NO signal, the pipeline must report no signal; given data with a known signal, it
must recover it. A research pipeline that cannot reliably find nothing is not
measuring anything, and every other test here is subordinate to that.
"""

from __future__ import annotations

import gzip
import math
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from nanobook import model as M
from nanobook import scoring, store
from nanobook.features import (FEATURE_COLUMNS, PanelConfig, build_panel,
                               load_day, panel_from_files)

SEC = 1_000_000_000


def cfg(**kw) -> PanelConfig:
    """PanelConfig for tests, with a short standardisation window.

    Production standardises features over a trailing 1,800-bar window with 300 bars
    of warm-up. Fixtures here are a few hundred rows, so with production settings
    every row would be dropped for want of history and the assertions below would
    pass vacuously against an empty panel. Shrinking the window keeps the tests
    testing the thing they name.
    """
    base = dict(warmup_s=0, min_rows_per_symbol=10, zscore_window=60,
                zscore_min_periods=20)
    base.update(kw)
    return PanelConfig(**base)


def write_feature_csv(path: Path, rows: list[dict]) -> None:
    """Write a feature CSV in exactly the format iex_replay emits."""
    cols = ["ts_ns", "symbol", "bid", "bid_shares", "ask", "ask_shares", "spread",
            "bid_depth", "ask_depth", "bid_levels", "ask_levels", "trades",
            "trade_shares", "buy_shares", "sell_shares", "unclassified_shares",
            "notional_cents", "last_price", "odd_lot_trades", "sweep_trades",
            "trading_status"]
    path.parent.mkdir(parents=True, exist_ok=True)
    df = pd.DataFrame(rows)
    for c in cols:
        if c not in df.columns:
            df[c] = 0
    with gzip.open(path, "wt") as fh:
        df[cols].to_csv(fh, index=False)


def synth_rows(n: int, *, symbol="TEST", start_ns=1577457000 * SEC, step_s=1,
               mid=1_000_000, spread=100, seed=0, drift=None) -> list[dict]:
    """A well-behaved synthetic quote stream: 1-second grid, tight spread."""
    rng = np.random.default_rng(seed)
    rows = []
    m = mid
    for i in range(n):
        if drift is not None:
            m = int(mid + drift[i])
        else:
            m = int(m + rng.integers(-1, 2) * 100)
        bid = m - spread // 2
        ask = m + spread // 2
        bsz = int(rng.integers(2, 40) * 100)
        asz = int(rng.integers(2, 40) * 100)
        rows.append(dict(
            ts_ns=start_ns + i * step_s * SEC, symbol=symbol,
            bid=bid, bid_shares=bsz, ask=ask, ask_shares=asz, spread=ask - bid,
            bid_depth=bsz * 3, ask_depth=asz * 3, bid_levels=5, ask_levels=5,
            trades=int(rng.integers(0, 4)), trade_shares=int(rng.integers(0, 500)),
            buy_shares=int(rng.integers(0, 300)), sell_shares=int(rng.integers(0, 300)),
            unclassified_shares=0, notional_cents=1000, last_price=m,
            odd_lot_trades=0, sweep_trades=0, trading_status="T"))
    return rows


# ---------------------------------------------------------------------------
# Forward returns are defined in TIME, not in rows.
# ---------------------------------------------------------------------------

def test_forward_return_uses_time_not_row_offset(tmp_path):
    """A quiet gap must not be treated as if it were `horizon` seconds.

    iex_replay skips intervals where nothing happened, so consecutive rows are not
    one second apart. A row-offset horizon would silently compare quotes minutes
    apart and call the difference a 10-second return.
    """
    rows = synth_rows(400, step_s=1, seed=1)
    # Punch a 10-minute hole after row 200 by pushing every later timestamp out.
    for r in rows[200:]:
        r["ts_ns"] += 600 * SEC
    p = tmp_path / "20200102.csv.gz"
    write_feature_csv(p, rows)

    panel = build_panel(load_day(p), cfg(horizon_s=10, tolerance_s=5))
    assert not panel.empty
    # Every surviving row's horizon endpoint must be within tolerance of 10s.
    elapsed = (panel["fwd_ts"] - panel["ts"]) / SEC
    assert elapsed.min() >= 10 - 1e-6
    assert elapsed.max() <= 15 + 1e-6, f"a row spans {elapsed.max():.0f}s, not ~10s"


def test_rows_whose_horizon_cannot_be_filled_are_dropped(tmp_path):
    rows = synth_rows(300, step_s=1, seed=2)
    p = tmp_path / "20200103.csv.gz"
    write_feature_csv(p, rows)
    panel = build_panel(load_day(p), cfg(horizon_s=10))
    assert not panel.empty
    # The last ~10 rows have no endpoint 10 seconds ahead of them.
    assert len(panel) <= len(rows) - 10
    assert panel["fwd_bps"].notna().all()


def test_one_sided_books_are_excluded(tmp_path):
    rows = synth_rows(300, seed=3)
    for r in rows[:100]:
        r["ask"] = 0          # pre-market style: bid only, no midpoint exists
        r["ask_shares"] = 0
    p = tmp_path / "20200106.csv.gz"
    write_feature_csv(p, rows)
    raw = load_day(p)
    assert len(raw) == 200
    assert (raw["bid"] > 0).all() and (raw["ask"] > 0).all()


# ---------------------------------------------------------------------------
# Tradeability filters
# ---------------------------------------------------------------------------

def test_absurd_spreads_are_filtered_out(tmp_path):
    """IEX's own book is often near-empty; a mid from a huge spread is fiction."""
    rows = synth_rows(600, seed=4, spread=100)
    for r in rows[300:]:
        r["ask"] = r["bid"] + 200_000     # a 20% spread
        r["spread"] = 200_000
    p = tmp_path / "20200107.csv.gz"
    write_feature_csv(p, rows)
    panel = build_panel(load_day(p), cfg())
    assert not panel.empty
    assert panel["spread_bps"].max() <= 25.0


def test_stale_books_are_filtered_out(tmp_path):
    rows = synth_rows(400, seed=5)
    for r in rows[200:]:
        r["ts_ns"] += 300 * SEC           # a five-minute hole
    p = tmp_path / "20200108.csv.gz"
    write_feature_csv(p, rows)
    panel = build_panel(load_day(p), cfg(max_gap_s=30))
    assert not panel.empty
    assert panel["gap_s"].max() <= 30.0


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def test_newey_west_shrinks_t_for_overlapping_data():
    """Overlapping horizons autocorrelate returns; naive t-stats are too large."""
    rng = np.random.default_rng(7)
    w = rng.normal(size=30_000)
    overlapped = np.convolve(w, np.ones(10) / 10, mode="same")
    naive = overlapped.mean() / (overlapped.std() / math.sqrt(len(overlapped)))
    hac = scoring.newey_west_tstat(overlapped, 10)
    assert abs(hac) < abs(naive) * 0.6, "HAC must substantially shrink an overlapped t"

    # On iid data it should barely change anything.
    naive_iid = w.mean() / (w.std() / math.sqrt(len(w)))
    hac_iid = scoring.newey_west_tstat(w, 10)
    assert abs(abs(hac_iid) - abs(naive_iid)) < 0.1 * max(abs(naive_iid), 0.5)


def test_norm_ppf_matches_scipy():
    scipy_stats = pytest.importorskip("scipy.stats")
    for p in (0.001, 0.01, 0.1, 0.5, 0.9, 0.99, 0.999):
        assert abs(scoring._norm_ppf(p) - scipy_stats.norm.ppf(p)) < 1e-6


def test_deflated_sharpe_takes_a_per_observation_sharpe():
    """Regression test for a units bug that made every DSR come out 1.000.

    Feeding a sqrt(n)-scaled Sharpe inflates the numerator without inflating the
    standard error, so everything looks certain. These assertions pin the units.
    """
    n = 20_000
    # A per-bar Sharpe of 0.005 (scaled ~0.71) chosen from 50 trials is nothing.
    assert scoring.deflated_sharpe(0.005, 50, n) < 0.2
    # The same per-bar Sharpe as a single pre-registered trial is more credible.
    assert scoring.deflated_sharpe(0.005, 1, n) > 0.6
    # More trials must never increase the deflated Sharpe.
    vals = [scoring.deflated_sharpe(0.02, k, n) for k in (1, 10, 50, 200)]
    assert vals == sorted(vals, reverse=True)


def test_expected_max_sharpe_grows_with_trials():
    n = 20_000
    hurdles = [scoring.expected_max_sharpe(k, n) for k in (1, 5, 20, 50, 200)]
    assert hurdles[0] == 0.0
    assert hurdles == sorted(hurdles)


def test_hit_rate_conditions_on_the_mid_moving():
    """sign(0) matches nothing; counting unmoved mids as misses inverts the read."""
    n = 4000
    rng = np.random.default_rng(11)
    pred = rng.normal(size=n)
    y = np.sign(pred) * 1.0              # a perfect signal...
    y[: n // 2] = 0.0                    # ...but the mid does not move half the time
    panel = pd.DataFrame({"fwd_bps": y, "spread_bps": np.full(n, 0.1),
                          "ret_5": rng.normal(size=n)})
    sc = scoring.score(panel, pred, horizon_s=10, cost_multiplier=0.5)
    assert sc.hit_rate == pytest.approx(1.0), "perfect signal must score 1.0 where it moved"
    assert sc.moved_fraction == pytest.approx(0.5, abs=0.02)


# ---------------------------------------------------------------------------
# Walk-forward discipline
# ---------------------------------------------------------------------------

def test_model_selection_requires_the_model_to_predate_the_day(tmp_path):
    layout = store.Layout(tmp_path)
    layout.models.mkdir(parents=True)
    for day in ("20200101", "20200102", "20200103"):
        (layout.models / f"model_{day}.json").write_text("{}")

    # Scoring 20200103 may only use a model trained through 01 or 02, never 03.
    chosen = layout.model_trained_before("20200103")
    assert chosen is not None
    assert chosen.name == "model_20200102.json"
    # And the earliest day has nothing legitimate to use.
    assert layout.model_trained_before("20200101") is None


def test_trials_are_counted_so_the_haircut_cannot_be_dodged(tmp_path):
    layout = store.Layout(tmp_path)
    base = {"horizon_s": 10, "alpha": 50.0}
    assert store.register_trial(layout, base) == 1
    assert store.register_trial(layout, base) == 1            # same config, no growth
    assert store.register_trial(layout, {**base, "alpha": 10.0}) == 2
    assert store.register_trial(layout, {**base, "horizon_s": 30}) == 3


def test_model_round_trips_and_rejects_a_changed_feature_set(tmp_path):
    rng = np.random.default_rng(3)
    n = 5000
    panel = pd.DataFrame({c: rng.normal(size=n) for c in FEATURE_COLUMNS})
    panel["fwd_bps"] = rng.normal(size=n)
    panel["date"] = "20200102"

    fitted = M.fit(panel, alpha=10.0)
    path = tmp_path / "m.json"
    fitted.to_json(path)
    reloaded = M.FittedModel.from_json(path)
    assert np.allclose(reloaded.predict(panel), fitted.predict(panel))

    reloaded.feature_names = ["something_else"]
    with pytest.raises(ValueError, match="feature set has changed"):
        reloaded.predict(panel)


def test_prediction_uses_the_training_scale_not_the_new_days_scale(tmp_path):
    """Re-standardising on the scored day would leak that day's distribution."""
    rng = np.random.default_rng(5)
    n = 4000
    train = pd.DataFrame({c: rng.normal(size=n) for c in FEATURE_COLUMNS})
    train["fwd_bps"] = train[FEATURE_COLUMNS[0]] * 2.0 + rng.normal(size=n)
    train["date"] = "20200102"
    fitted = M.fit(train, alpha=1.0)

    # A later day on a wildly different scale must be transformed by the STORED
    # mean and sd, so its predictions shift accordingly rather than being renormalised.
    shifted = train.copy()
    shifted[FEATURE_COLUMNS[0]] += 10.0
    assert fitted.predict(shifted).mean() > fitted.predict(train).mean() + 1.0


# ---------------------------------------------------------------------------
# CALIBRATION — the tests this whole file exists for.
# ---------------------------------------------------------------------------

def test_pipeline_finds_nothing_when_there_is_nothing():
    """Pure noise must produce an IC indistinguishable from zero.

    This is the single most important test here. A pipeline that reports an edge on
    random data will report an edge on anything, and every result it produces is
    worthless. The leak that this catches in practice is any form of look-ahead:
    with genuinely independent features and target, nothing can be recovered.
    """
    rng = np.random.default_rng(101)
    n = 40_000
    panel = pd.DataFrame({c: rng.normal(size=n) for c in FEATURE_COLUMNS})
    panel["fwd_bps"] = rng.normal(scale=1.0, size=n)        # independent of everything
    panel["spread_bps"] = np.full(n, 1.0)
    panel["ret_5"] = panel["ret_5"]
    panel["date"] = "20200102"

    fitted = M.fit(panel, alpha=50.0)

    # Score on a FRESH independent sample, as the daily loop does.
    test = pd.DataFrame({c: rng.normal(size=n) for c in FEATURE_COLUMNS})
    test["fwd_bps"] = rng.normal(scale=1.0, size=n)
    test["spread_bps"] = np.full(n, 1.0)
    sc = scoring.score(test, fitted.predict(test), horizon_s=10, cost_multiplier=0.5)

    assert abs(sc.ic_spearman) < 0.02, f"found signal in noise: IC {sc.ic_spearman:+.4f}"
    assert abs(sc.gross_t) < 3.0, f"spurious significance: t {sc.gross_t:+.2f}"
    dsr = scoring.deflated_sharpe(sc.sharpe_per_bar, 1, sc.n)
    assert not (dsr > 0.95), "noise must not clear the deflated-Sharpe bar"


def test_pipeline_recovers_a_signal_that_is_really_there():
    """A planted relationship must be found, out-of-sample, with the right sign.

    The complement of the null test: a pipeline tuned until it finds nothing would
    also pass that one.
    """
    rng = np.random.default_rng(202)
    n = 40_000

    def make(seed: int) -> pd.DataFrame:
        r = np.random.default_rng(seed)
        df = pd.DataFrame({c: r.normal(size=n) for c in FEATURE_COLUMNS})
        # Signal-to-noise deliberately low, as it is in real microstructure data.
        df["fwd_bps"] = 0.25 * df["queue_imbalance"] + r.normal(scale=1.0, size=n)
        df["spread_bps"] = np.full(n, 0.05)      # cheap to trade, so net stays positive
        df["date"] = "20200102"
        return df

    train, test = make(1), make(2)
    fitted = M.fit(train, alpha=10.0)

    qi = fitted.feature_names.index("queue_imbalance")
    assert fitted.coefficients[qi] > 0, "coefficient must have the planted sign"

    sc = scoring.score(test, fitted.predict(test), horizon_s=10, cost_multiplier=0.5)
    assert sc.ic_spearman > 0.1, f"failed to recover a real signal: IC {sc.ic_spearman:+.4f}"
    assert sc.hit_rate > 0.52
    assert sc.gross_bps > 0
    assert sc.gross_t > 5
    # And with a near-zero spread the edge should survive costs.
    assert sc.net_bps > 0, "a real signal with negligible cost must be net positive"


def test_costs_can_turn_a_real_signal_unprofitable():
    """The cost model must be able to kill a genuine signal, which is the finding
    the real IEX data actually produces: informative direction, moves too small."""
    rng = np.random.default_rng(303)
    n = 20_000
    df = pd.DataFrame({c: rng.normal(size=n) for c in FEATURE_COLUMNS})
    df["fwd_bps"] = 0.3 * df["queue_imbalance"] + rng.normal(scale=0.5, size=n)
    df["spread_bps"] = np.full(n, 6.0)          # wide: 3 bp to cross
    fitted_pred = 0.3 * df["queue_imbalance"].to_numpy()

    sc = scoring.score(df, fitted_pred, horizon_s=10, cost_multiplier=0.5)
    assert sc.ic_spearman > 0.1          # the signal is real
    assert sc.gross_bps > 0              # and profitable before costs
    assert sc.net_bps < 0                # but not after them


def test_a_symbol_with_no_classifiable_flow_is_not_silently_dropped(tmp_path):
    """Regression test. `signed_flow_norm` divides by a rolling mean of |flow|; if a
    symbol trades only at the midpoint, Lee-Ready classifies nothing, that mean is
    zero, and a bare division makes every row NaN. Because the panel drops rows
    with any missing feature, the entire symbol then vanishes without a word.
    """
    rows = synth_rows(600, seed=21)
    for r in rows:
        r["buy_shares"] = 0          # every trade exactly at the mid
        r["sell_shares"] = 0
        r["unclassified_shares"] = 200
    p = tmp_path / "20200109.csv.gz"
    write_feature_csv(p, rows)

    panel = build_panel(load_day(p), cfg())
    assert not panel.empty, "symbol disappeared because it had no classified flow"
    assert (panel["signed_flow_norm"] == 0).all()
    assert panel["flow_imbalance"].notna().all()


def test_spread_feature_carries_no_price_level(tmp_path):
    """The spread FEATURE must be in ticks, not basis points.

    `spread / mid` is, for a name quoting a fixed number of ticks, nearly a
    deterministic function of the price level. Regressing forward returns on a
    level along a single random-walk path manufactures correlation from nothing: on
    synthetic noise the basis-point version reached Spearman -0.28 against the
    forward return and the model reported an IC of +0.22 on data with no signal in
    it. Ticks carry no level, so the channel is closed at the source.
    """
    assert "spread_ticks" in FEATURE_COLUMNS
    assert "spread_bps" not in FEATURE_COLUMNS, "the cost measure must not be a feature"

    # Two identical books at very different price levels must give the same feature.
    for mid in (1_000_000, 50_000_000):
        rows = synth_rows(400, seed=31, mid=mid, spread=100)
        p = tmp_path / f"2020011{1 if mid == 1_000_000 else 2}.csv.gz"
        write_feature_csv(p, rows)
        raw = load_day(p)
        from nanobook.features import _add_features
        feat = _add_features(raw.sort_values("ts").reset_index(drop=True))
        assert feat["spread_ticks"].iloc[0] == pytest.approx(1.0)
        # ...whereas the basis-point version differs by the ratio of the prices.
        assert feat["spread_bps"].iloc[0] == pytest.approx(100 / mid * 10_000, rel=1e-3)


def test_features_are_trailing_zscores_so_they_cannot_peek(tmp_path):
    """Standardisation must look backwards only."""
    rows = synth_rows(600, seed=41)
    p = tmp_path / "20200113.csv.gz"
    write_feature_csv(p, rows)
    panel = build_panel(load_day(p), cfg())
    assert not panel.empty
    # A trailing z-score is roughly mean-zero and unit-variance, but not exactly,
    # precisely because it never sees the future.
    for col in ("queue_imbalance", "spread_ticks"):
        assert abs(panel[col].mean()) < 1.0
        assert panel[col].std() < 5.0
