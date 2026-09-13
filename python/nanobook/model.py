"""Walk-forward ridge model, serialised so out-of-sample-ness is auditable.

The point of writing the model to disk is not convenience. A model file for date D
is fitted only on data strictly before D, and it is written before day D's data
exists at all. Its predictions for D are therefore out-of-sample in the strong
sense: not "held out from a shuffle", but "made before the outcome was knowable".
That is the claim a backtest cannot make, and keeping the fitted coefficients and
the training-set fingerprint on disk is what lets someone else check it.

Ridge rather than anything fancier, deliberately. With ~11 collinear microstructure
features and a target that is mostly noise, a gradient-boosted forest would fit the
noise more confidently and be harder to sanity-check. A linear model's coefficients
can be read and argued with.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd

from .features import FEATURE_COLUMNS

SCHEMA_VERSION = 1


@dataclass
class FittedModel:
    """A ridge fit plus everything needed to audit it."""

    trained_through: str                 # last date in the training set (YYYYMMDD)
    train_dates: list[str]
    n_train_rows: int
    feature_names: list[str]
    coefficients: list[float]
    intercept: float
    # Standardisation is stored, not recomputed, so prediction on a later day uses
    # exactly the scale the fit saw. Recomputing it from the new day's data would
    # leak that day's distribution into its own predictions.
    feature_mean: list[float]
    feature_std: list[float]
    target_std: float
    alpha: float
    train_fingerprint: str
    schema_version: int = SCHEMA_VERSION
    in_sample_ic: float = 0.0
    notes: dict = field(default_factory=dict)

    def to_json(self, path: Path) -> None:
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_text(json.dumps(asdict(self), indent=2, sort_keys=True))

    @staticmethod
    def from_json(path: Path) -> "FittedModel":
        d = json.loads(Path(path).read_text())
        if d.get("schema_version") != SCHEMA_VERSION:
            raise ValueError(
                f"model {path} has schema version {d.get('schema_version')}, "
                f"expected {SCHEMA_VERSION}"
            )
        d.pop("schema_version", None)
        return FittedModel(schema_version=SCHEMA_VERSION, **d)

    def predict(self, panel: pd.DataFrame) -> np.ndarray:
        """Predicted forward return in basis points."""
        if panel.empty:
            return np.array([])
        if list(self.feature_names) != list(FEATURE_COLUMNS):
            raise ValueError(
                "feature set has changed since this model was fitted; "
                f"model has {self.feature_names}, code has {FEATURE_COLUMNS}"
            )
        x = panel[self.feature_names].to_numpy(dtype="float64")
        mean = np.asarray(self.feature_mean)
        std = np.asarray(self.feature_std)
        z = (x - mean) / np.where(std > 0, std, 1.0)
        return (z @ np.asarray(self.coefficients) + self.intercept) * self.target_std


def _fingerprint(panel: pd.DataFrame) -> str:
    """Cheap, stable fingerprint of the training set, for audit trails."""
    h = hashlib.sha256()
    h.update(str(len(panel)).encode())
    for col in ("date", "symbol"):
        if col in panel.columns:
            vals = sorted(panel[col].astype(str).unique())
            h.update("|".join(vals).encode())
    # Include the target's moments so a silent change in preprocessing shows up.
    y = panel["fwd_bps"].to_numpy(dtype="float64")
    h.update(f"{y.mean():.9f}:{y.std():.9f}".encode())
    return h.hexdigest()[:16]


def fit(panel: pd.DataFrame, alpha: float = 50.0) -> FittedModel:
    """Fit ridge on a standardised panel.

    Standardising both sides makes `alpha` comparable across days and makes the
    coefficients readable as "basis points of target per standard deviation of
    feature", which is the only form in which they can be sanity-checked.
    """
    if panel.empty:
        raise ValueError("cannot fit on an empty panel")

    x = panel[FEATURE_COLUMNS].to_numpy(dtype="float64")
    y = panel["fwd_bps"].to_numpy(dtype="float64")

    mean = x.mean(axis=0)
    std = x.std(axis=0)
    z = (x - mean) / np.where(std > 0, std, 1.0)

    y_std = float(y.std())
    if y_std <= 0:
        raise ValueError("target has zero variance; nothing to fit")
    yz = y / y_std

    # Closed-form ridge. The intercept is not penalised, which is why it is fitted
    # separately from the centred system rather than by appending a column of ones.
    n_feat = z.shape[1]
    gram = z.T @ z + alpha * np.eye(n_feat)
    coef = np.linalg.solve(gram, z.T @ (yz - yz.mean()))
    intercept = float(yz.mean())

    pred = z @ coef + intercept
    ic = float(pd.Series(pred).corr(pd.Series(yz), method="spearman"))

    return FittedModel(
        trained_through=str(panel["date"].max()) if "date" in panel else "",
        train_dates=sorted(panel["date"].astype(str).unique()) if "date" in panel else [],
        n_train_rows=int(len(panel)),
        feature_names=list(FEATURE_COLUMNS),
        coefficients=[float(c) for c in coef],
        intercept=intercept,
        feature_mean=[float(v) for v in mean],
        feature_std=[float(v) for v in std],
        target_std=y_std,
        alpha=float(alpha),
        train_fingerprint=_fingerprint(panel),
        in_sample_ic=0.0 if np.isnan(ic) else ic,
    )
