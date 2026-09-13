"""Scoring that tries hard not to flatter the signal.

Four things here exist specifically because omitting them is how microstructure
research fools itself:

1. NEWEY-WEST STANDARD ERRORS. Forward returns at overlapping horizons are
   autocorrelated by construction: a 10-second target sampled every second shares
   9 seconds of its path with its neighbour. Ordinary standard errors are then far
   too small and a t-stat of 6 can be pure overlap. The HAC correction is not
   optional.

2. COST-AWARE PnL. A signal that predicts a 0.4 bp move is worthless if crossing
   the spread costs 1.3 bp. The scorecard reports gross and net, and the net
   number assumes the strategy pays half the quoted spread per side — optimistic
   for a taker, pessimistic for a patient maker, and stated either way.

3. MULTIPLE-TESTING AWARENESS. Every configuration ever scored is counted, and the
   deflated Sharpe ratio adjusts the best observed result for how many were tried.
   A Sharpe of 2 chosen from 50 attempts is not a Sharpe of 2.

4. A BASELINE THAT IS HARD TO BEAT. Predicting zero always is scored alongside, as
   is pure short-horizon reversal. A signal that cannot beat "the mid does not
   move" has found nothing.
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass

import numpy as np
import pandas as pd


@dataclass
class Scorecard:
    n: int
    ic_pearson: float
    ic_spearman: float
    hit_rate: float         # among bars where the mid actually MOVED
    moved_fraction: float   # ...and how often that was
    # Mean realised return of a unit-sign position, in basis points.
    gross_bps: float
    net_bps: float
    gross_t: float          # Newey-West corrected
    net_t: float
    sharpe_per_bar: float   # mean/sd of the net per-bar series (DSR takes THIS)
    sharpe_daily: float     # sharpe_per_bar * sqrt(bars in the sample)
    mean_spread_bps: float
    # Fraction of predictions large enough to clear half the spread.
    tradeable_fraction: float
    baseline_zero_bps: float
    baseline_reversal_ic: float

    def as_dict(self) -> dict:
        return asdict(self)


def newey_west_tstat(x: np.ndarray, lags: int) -> float:
    """t-statistic of the mean of `x` with a Newey-West HAC variance estimate.

    Bartlett kernel. `lags` should be at least the overlap of the target horizon,
    since that is the mechanical source of the autocorrelation.
    """
    x = np.asarray(x, dtype="float64")
    x = x[np.isfinite(x)]
    n = len(x)
    if n < 30:
        return float("nan")
    mu = x.mean()
    e = x - mu
    gamma0 = float(e @ e) / n
    var = gamma0
    for lag in range(1, min(lags, n - 1) + 1):
        cov = float(e[lag:] @ e[:-lag]) / n
        weight = 1.0 - lag / (lags + 1.0)
        var += 2.0 * weight * cov
    if var <= 0:
        return float("nan")
    return mu / math.sqrt(var / n)


def expected_max_sharpe(n_trials: int, n_obs: int) -> float:
    """Per-observation Sharpe the BEST of `n_trials` pure-noise strategies would show.

    Bailey & Lopez de Prado: the expected maximum of N standard normals is
    (1-gamma)*Phi^-1(1 - 1/N) + gamma*Phi^-1(1 - 1/(N*e)), scaled by the standard
    error of a Sharpe estimate. With 50 trials and 20,000 observations this is
    about 0.016 per bar — which is the bar any real signal has to clear.
    """
    if n_trials < 1 or n_obs < 10:
        return float("nan")
    if n_trials == 1:
        return 0.0
    euler = 0.5772156649015329
    e_max = ((1 - euler) * _norm_ppf(1 - 1.0 / n_trials)
             + euler * _norm_ppf(1 - 1.0 / (n_trials * math.e)))
    return e_max / math.sqrt(n_obs)


def deflated_sharpe(sharpe_per_obs: float, n_trials: int, n_obs: int) -> float:
    """Bailey & Lopez de Prado haircut for selection across trials.

    IMPORTANT — UNITS. `sharpe_per_obs` must be the PER-OBSERVATION Sharpe
    (mean / sd of the per-bar return series), not an annualised or sqrt(n)-scaled
    one. Passing a scaled Sharpe here makes every result look certain, because the
    numerator is then inflated by sqrt(n) while the standard error is not. That is
    exactly the mistake this docstring exists to prevent.

    Returns the probability that the observed Sharpe is better than the best of
    `n_trials` noise strategies. Below ~0.95, the result is not distinguishable
    from the luckiest of many coin flips.
    """
    if not math.isfinite(sharpe_per_obs) or n_trials < 1 or n_obs < 10:
        return float("nan")
    # Variance of a Sharpe estimate under iid normal returns. The SR^2/2 term
    # matters once the Sharpe is large; for microstructure per-bar Sharpes it is
    # negligible, but leaving it out is wrong rather than merely approximate.
    var = (1.0 + 0.5 * sharpe_per_obs ** 2) / n_obs
    se = math.sqrt(var)
    if se <= 0:
        return float("nan")
    return _norm_cdf((sharpe_per_obs - expected_max_sharpe(n_trials, n_obs)) / se)


def _norm_cdf(z: float) -> float:
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))


def _norm_ppf(p: float) -> float:
    """Inverse normal CDF, Acklam's rational approximation (|err| < 1.2e-9)."""
    if not 0.0 < p < 1.0:
        return float("nan")
    a = [-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
         1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00]
    b = [-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
         6.680131188771972e+01, -1.328068155288572e+01]
    c = [-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
         -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00]
    d = [7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
         3.754408661907416e+00]
    plow, phigh = 0.02425, 1 - 0.02425
    if p < plow:
        q = math.sqrt(-2 * math.log(p))
        return (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / \
               ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    if p > phigh:
        q = math.sqrt(-2 * math.log(1 - p))
        return -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / \
                ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    q = p - 0.5
    r = q * q
    return (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q / \
           (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1)


def score(panel: pd.DataFrame, pred: np.ndarray, horizon_s: int = 10,
          cost_multiplier: float = 0.5) -> Scorecard:
    """Score predictions against realised forward returns.

    `cost_multiplier` is the fraction of the quoted spread paid per round trip.
    0.5 means crossing half the spread — a taker paying one side.
    """
    y = panel["fwd_bps"].to_numpy(dtype="float64")
    spread = panel["spread_bps"].to_numpy(dtype="float64")
    pred = np.asarray(pred, dtype="float64")

    ok = np.isfinite(y) & np.isfinite(pred) & np.isfinite(spread)
    y, pred, spread = y[ok], pred[ok], spread[ok]
    n = len(y)
    if n < 50:
        return Scorecard(n, *([float("nan")] * 14))

    ic_p = float(np.corrcoef(pred, y)[0, 1]) if np.std(pred) > 0 else float("nan")
    ic_s = float(pd.Series(pred).corr(pd.Series(y), method="spearman"))

    sign = np.sign(pred)
    nonzero = sign != 0
    # Over a 10-second horizon the midpoint frequently does not move at all, and
    # sign(0) matches no prediction. Counting those as misses drags the hit rate
    # below 0.5 even for a genuinely informative signal, which reads as if the
    # model were inverted. Condition on the mid having moved, and report separately
    # how often it did.
    moved = nonzero & (y != 0)
    hit = float(np.mean(np.sign(y[moved]) == sign[moved])) if moved.any() else float("nan")
    moved_frac = float(np.mean(y[nonzero] != 0)) if nonzero.any() else float("nan")

    # A unit position in the predicted direction. Gross is the realised move;
    # net pays the spread cost on every position taken.
    gross = sign * y
    cost = cost_multiplier * spread
    net = gross - np.where(nonzero, cost, 0.0)

    # Overlapping targets: the HAC lag must cover the horizon. Bars are ~1s.
    lags = max(horizon_s, 10)
    gross_t = newey_west_tstat(gross, lags)
    net_t = newey_west_tstat(net, lags)

    # Two Sharpes, because conflating them is how a deflated-Sharpe calculation
    # silently returns 1.0 for everything: the per-bar figure is what the
    # statistics operate on, the scaled one is what is human-readable.
    sd = float(np.std(net))
    sharpe_bar = (float(np.mean(net)) / sd) if sd > 0 else float("nan")
    sharpe_scaled = sharpe_bar * math.sqrt(max(n, 1)) if math.isfinite(sharpe_bar) else float("nan")

    # How often is the prediction even big enough to pay for itself?
    tradeable = float(np.mean(np.abs(pred) > cost))

    # Baselines. Reversal: yesterday's short-horizon move, negated.
    rev = -panel["ret_5"].to_numpy(dtype="float64")[ok]
    rev_ic = float(pd.Series(rev).corr(pd.Series(y), method="spearman"))

    return Scorecard(
        n=n,
        ic_pearson=ic_p,
        ic_spearman=ic_s,
        hit_rate=hit,
        moved_fraction=moved_frac,
        gross_bps=float(np.mean(gross)),
        net_bps=float(np.mean(net)),
        gross_t=gross_t,
        net_t=net_t,
        sharpe_per_bar=sharpe_bar,
        sharpe_daily=sharpe_scaled,
        mean_spread_bps=float(np.mean(spread)),
        tradeable_fraction=tradeable,
        baseline_zero_bps=0.0,
        baseline_reversal_ic=rev_ic,
    )
