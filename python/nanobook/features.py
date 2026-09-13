"""Build a model matrix from an iex_replay feature table.

Two design decisions here carry all the research validity.

1. TARGETS ARE DEFINED IN TIME, NOT IN ROWS.

   iex_replay skips intervals in which nothing happened, so consecutive rows are
   NOT one second apart. Using `df.shift(-10)` for a 10-second horizon would
   silently compare quotes that are minutes apart across a quiet stretch, and the
   resulting "returns" would be dominated by gaps rather than by anything the
   features could predict. Forward returns are therefore looked up by timestamp,
   with a staleness tolerance, and rows whose horizon cannot be filled are dropped
   rather than approximated.

2. ONLY TWO-SIDED, TRADEABLE BARS SURVIVE.

   A one-sided book has no midpoint. IEX pre-market is full of them, and a
   midpoint fabricated from one side would look like a violent price move at the
   open. Bars without both a bid and an ask are dropped.

Prices arrive as integers in 1/10000 of a dollar and stay integral until the last
possible moment; returns are computed in basis points off the integer midpoint.
"""

from __future__ import annotations

import gzip
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd

PRICE_SCALE = 10_000

# Feature columns handed to the model, in a fixed order so a serialised model's
# coefficients keep their meaning across runs.
FEATURE_COLUMNS = [
    "queue_imbalance",
    "depth_imbalance",
    "flow_imbalance",
    "signed_flow_norm",
    "spread_ticks",
    "trade_intensity",
    "ret_1",
    "ret_5",
    "ret_30",
    "vol_30",
    "depth_ratio",
]


@dataclass(frozen=True)
class PanelConfig:
    horizon_s: int = 10          # forward return horizon
    # Trailing window for per-symbol feature standardisation, in bars. See
    # _standardise below for why this is not optional.
    zscore_window: int = 1800
    zscore_min_periods: int = 300
    tolerance_s: int = 5         # how stale the horizon endpoint may be
    warmup_s: int = 300          # skip the first N seconds of quoting per symbol
    min_rows_per_symbol: int = 200

    # --- tradeability filters ---
    #
    # These are not cosmetic. IEX is one venue with ~2-5% of volume, so its own
    # book is frequently near-empty even in large names, and the "midpoint" of a
    # $200 bid against a $280 ask is fiction. Unfiltered, those bars dominate: in
    # one December 2019 session the 99th percentile quoted spread was 2,331 bp and
    # forward returns had a standard deviation of 232 bp over ten seconds. Both are
    # artefacts of an empty book, not market moves, and they swamp any real signal.
    max_spread_bps: float = 25.0      # absolute cap on the quoted spread
    max_spread_ratio: float = 4.0     # and no wider than N x the symbol's own median
    max_gap_s: float = 30.0           # book must have updated recently to be live
    min_touch_shares: int = 100       # at least a round lot resting on each side
    # Regular US equity session in UTC nanoseconds-of-day terms is handled via the
    # exchange calendar below; auctions are excluded because their dynamics are
    # entirely different from continuous trading.
    session_only: bool = True


# 09:30-16:00 America/New_York. Stored as seconds since midnight UTC for the two
# possible offsets; December is EST (UTC-5), summer is EDT (UTC-4).
_SESSION_EST = (14 * 3600 + 30 * 60, 21 * 3600)
_SESSION_EDT = (13 * 3600 + 30 * 60, 20 * 3600)


def _read_csv(path: Path) -> pd.DataFrame:
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt") as fh:
        return pd.read_csv(fh)


def load_day(path: Path) -> pd.DataFrame:
    """Load one day's feature CSV and attach derived columns."""
    df = _read_csv(Path(path))
    if df.empty:
        return df

    df["ts"] = df["ts_ns"].astype("int64")
    # Drop bars without a two-sided book: no midpoint exists for them.
    df = df[(df["bid"] > 0) & (df["ask"] > 0)].copy()
    if df.empty:
        return df

    # Integer midpoint, doubled, so a half-cent mid is exact.
    df["mid_x2"] = df["bid"].astype("int64") + df["ask"].astype("int64")
    df["spread"] = df["ask"].astype("int64") - df["bid"].astype("int64")

    # A crossed book after a completed transaction should not happen on one venue;
    # if it does, the row is not trustworthy.
    df = df[df["spread"] >= 0].copy()
    return df


def _session_window(ts_ns: int) -> tuple[int, int]:
    """Regular-session bounds, in seconds since midnight UTC, for this timestamp."""
    month = pd.Timestamp(ts_ns, unit="ns", tz="UTC").month
    # Crude but adequate: US DST runs mid-March to early November.
    return _SESSION_EST if month in (12, 1, 2, 11) else _SESSION_EDT


def build_panel(df: pd.DataFrame, cfg: PanelConfig = PanelConfig()) -> pd.DataFrame:
    """Add features and the forward-return target; returns the usable rows only."""
    if df.empty:
        return df

    out = []
    for symbol, g in df.groupby("symbol", sort=True):
        g = g.sort_values("ts").reset_index(drop=True)

        if cfg.session_only:
            lo, hi = _session_window(int(g["ts"].iloc[0]))
            sod = (g["ts"] // 86_400_000_000_000) * 86_400_000_000_000
            secs = (g["ts"] - sod) // 1_000_000_000
            g = g[(secs >= lo) & (secs < hi)].reset_index(drop=True)

        if len(g) < cfg.min_rows_per_symbol:
            continue

        # Skip the opening warmup: the book is still filling out and the first
        # quotes of the session are not representative.
        g = g[g["ts"] >= g["ts"].iloc[0] + cfg.warmup_s * 1_000_000_000].reset_index(drop=True)
        if len(g) < cfg.min_rows_per_symbol:
            continue

        g = _add_features(g)
        g = _standardise(g, cfg)
        g = _filter_tradeable(g, cfg)
        if len(g) < cfg.min_rows_per_symbol:
            continue
        g = _add_forward_return(g, cfg)
        g["symbol"] = symbol
        out.append(g)

    if not out:
        return pd.DataFrame(columns=list(df.columns) + FEATURE_COLUMNS + ["fwd_bps"])
    panel = pd.concat(out, ignore_index=True)
    panel = panel.dropna(subset=FEATURE_COLUMNS + ["fwd_bps"]).reset_index(drop=True)
    return panel


def _safe_imbalance(a: pd.Series, b: pd.Series) -> pd.Series:
    """(a - b) / (a + b), zero when both sides are empty."""
    total = a.astype("float64") + b.astype("float64")
    return np.where(total > 0, (a - b) / total.replace(0, np.nan), 0.0)


def _add_features(g: pd.DataFrame) -> pd.DataFrame:
    g = g.copy()
    mid = g["mid_x2"].astype("float64") / 2.0

    # Book pressure at the touch and a few ticks deep. These are the workhorse
    # microstructure predictors: the side with more resting size tends to be the
    # side the midpoint moves away from.
    g["queue_imbalance"] = _safe_imbalance(g["bid_shares"], g["ask_shares"])
    g["depth_imbalance"] = _safe_imbalance(g["bid_depth"], g["ask_depth"])

    # Trade flow imbalance from the Lee-Ready classification done in C++.
    g["flow_imbalance"] = _safe_imbalance(g["buy_shares"], g["sell_shares"])
    signed = g["buy_shares"].astype("float64") - g["sell_shares"].astype("float64")
    # Normalise signed flow by its own trailing scale so it is comparable across
    # symbols with very different volumes.
    #
    # The scale is zero whenever a symbol has had no classifiable flow in the
    # trailing window — which happens for real: a name can go minutes with trades
    # only exactly at the midpoint, and Lee-Ready leaves those unclassified. A bare
    # division then yields NaN for every row, and because the panel drops rows with
    # any missing feature, the ENTIRE SYMBOL silently disappears. "No flow" is
    # information, not absence of data, so it maps to zero.
    scale = signed.abs().rolling(300, min_periods=30).mean()
    g["signed_flow_norm"] = np.where(
        (scale > 0) & np.isfinite(scale), signed / scale.where(scale > 0, 1.0), 0.0
    ).clip(-10, 10)

    # TWO spread columns, deliberately, because they answer different questions.
    #
    # `spread_ticks` is the FEATURE. It carries no price level, so it cannot
    # smuggle one into the regression. That matters: `spread / mid` is, for a name
    # that usually quotes a fixed number of ticks, very nearly a deterministic
    # function of the price, and regressing forward returns on a price level along
    # a single random-walk path manufactures correlation from nothing. On synthetic
    # noise the basis-point version reached a Spearman correlation of -0.28 with
    # the forward return and led the model to report an IC of +0.22 on data with no
    # signal in it at all.
    #
    # `spread_bps` is the COST, and is not a feature. Economically the cost of
    # crossing is relative, so basis points are right there — but it is only ever
    # read by the scorer, never fitted on.
    #
    # The tick is assumed to be one cent, which SEC Reg NMS Rule 612 makes correct
    # for any stock at or above $1.00; sub-dollar names need the ladder and this
    # divisor changed together.
    g["spread_ticks"] = g["spread"].astype("float64") / 100.0
    g["spread_bps"] = g["spread"].astype("float64") / mid * 10_000.0
    g["trade_intensity"] = g["trades"].astype("float64")
    touch = (g["bid_shares"].astype("float64") + g["ask_shares"].astype("float64"))
    g["depth_ratio"] = np.where(
        touch > 0,
        (g["bid_depth"].astype("float64") + g["ask_depth"].astype("float64"))
        / touch.where(touch > 0, 1.0),
        0.0,
    ).clip(0, 100)

    # Trailing returns, in basis points. Momentum at these horizons is usually
    # reversal, which is itself the signal.
    lm = np.log(mid)
    for n in (1, 5, 30):
        g[f"ret_{n}"] = (lm - lm.shift(n)) * 10_000.0
    # Volatility is undefined until the window fills; zero is the honest value for
    # "no observed variation yet" and keeps the early rows from vanishing.
    g["vol_30"] = g["ret_1"].rolling(30, min_periods=10).std().fillna(0.0)
    return g


def _standardise(g: pd.DataFrame, cfg: PanelConfig) -> pd.DataFrame:
    """Replace each feature with a TRAILING z-score, computed per symbol.

    This is not cosmetic tidying; it closes a spurious-regression channel.

    `spread_bps` is spread / mid, so for a symbol whose quoted spread is usually a
    fixed number of ticks it is very nearly a deterministic function of the PRICE
    LEVEL. Regressing forward returns on a price level along a single random-walk
    path produces correlation out of nothing — the classic integrated-variable
    regression problem. Measured on a synthetic driftless random walk with no
    signal whatsoever, raw `spread_bps` reached a Spearman correlation of -0.28
    with the forward return, and the fitted model duly "found" an information
    coefficient of +0.22 in pure noise. `vol_30`, `depth_ratio` and
    `trade_intensity` carry the same level and scale dependence more weakly.

    A trailing z-score makes each feature stationary and comparable across symbols
    while keeping what actually matters — "the spread is unusually wide *for this
    name, right now*" — and it cannot leak, because the window only ever looks
    backwards. Rows before the window fills are dropped rather than imputed.
    """
    g = g.copy()
    for col in FEATURE_COLUMNS:
        roll = g[col].rolling(cfg.zscore_window, min_periods=cfg.zscore_min_periods)
        mu = roll.mean()
        sd = roll.std()
        # A constant feature has no information; zero rather than divide by zero.
        g[col] = np.where(sd > 0, (g[col] - mu) / sd.where(sd > 0, 1.0), 0.0)
        g[col] = pd.Series(g[col], index=g.index).where(mu.notna())
    return g


def _filter_tradeable(g: pd.DataFrame, cfg: PanelConfig) -> pd.DataFrame:
    """Keep only bars where the quote could plausibly have been traded against.

    Applied AFTER features are computed, so trailing returns and volatility are
    built from the full quote history rather than from a gappy filtered subset —
    filtering first would make `ret_30` span arbitrary stretches of time.
    """
    g = g.copy()
    # Seconds since the previous update: a book nobody has touched in minutes is
    # not a live market, whatever its last quote said.
    g["gap_s"] = g["ts"].diff().fillna(0) / 1e9

    # Expanding rather than full-day median. Using the whole session's median to
    # decide which of its own bars are tradeable is a look-ahead in sample
    # SELECTION: late-session volatility would change which early bars are kept.
    # The effect is small, but "small look-ahead" is not a category worth having.
    running = g["spread_bps"].expanding(min_periods=30).median()
    ceiling = np.minimum(cfg.max_spread_bps,
                         cfg.max_spread_ratio * running.fillna(cfg.max_spread_bps))

    keep = (
        (g["spread_bps"] > 0)
        & (g["spread_bps"] <= ceiling)
        & (g["gap_s"] <= cfg.max_gap_s)
        & (g["bid_shares"] >= cfg.min_touch_shares)
        & (g["ask_shares"] >= cfg.min_touch_shares)
    )
    return g[keep].reset_index(drop=True)


def _add_forward_return(g: pd.DataFrame, cfg: PanelConfig) -> pd.DataFrame:
    """Forward midpoint return, looked up by TIME rather than by row offset.

    For each row at t, find the most recent row at or after t + horizon. If that
    row is more than `tolerance` beyond the horizon, the market went quiet and
    there is no honest observation to score; the row is dropped.
    """
    g = g.copy()
    ts = g["ts"].to_numpy()
    mid = g["mid_x2"].to_numpy().astype("float64") / 2.0

    horizon = cfg.horizon_s * 1_000_000_000
    tol = cfg.tolerance_s * 1_000_000_000

    # searchsorted gives the first row at or after the target time.
    target = ts + horizon
    idx = np.searchsorted(ts, target, side="left")
    valid = idx < len(ts)

    fwd = np.full(len(ts), np.nan)
    j = idx[valid]
    # Reject endpoints that overshoot the horizon by more than the tolerance.
    ok = ts[j] <= target[valid] + tol
    rows = np.flatnonzero(valid)[ok]
    cols = j[ok]
    fwd[rows] = np.log(mid[cols] / mid[rows]) * 10_000.0

    g["fwd_bps"] = fwd
    g["fwd_ts"] = np.where(valid, ts[np.clip(idx, 0, len(ts) - 1)], np.nan)
    return g


def panel_from_files(paths: list[Path], cfg: PanelConfig = PanelConfig()) -> pd.DataFrame:
    """Concatenate several days into one panel, tagging each row with its date."""
    frames = []
    for p in paths:
        raw = load_day(Path(p))
        if raw.empty:
            continue
        panel = build_panel(raw, cfg)
        if panel.empty:
            continue
        panel["date"] = Path(p).name.split(".")[0]
        frames.append(panel)
    if not frames:
        return pd.DataFrame()
    return pd.concat(frames, ignore_index=True)
