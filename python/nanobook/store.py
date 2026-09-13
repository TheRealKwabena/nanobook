"""On-disk layout for the daily loop, and the audit trail that makes it credible.

    data/features/YYYYMMDD.csv.gz     feature table from iex_replay
    data/models/model_YYYYMMDD.json   ridge fit using data through that date
    data/scorecard.jsonl              one append-only record per scored day
    data/trials.json                  every configuration ever evaluated

The append-only scorecard is the point. Each record says which model file made the
predictions and what its training fingerprint was, so anyone can check that the
model predates the day it was scored on. A backtest can always be re-run until it
looks good; an append-only log of predictions made before the outcome existed
cannot.

`trials.json` exists to keep the multiple-testing correction honest. Every distinct
configuration that has ever been scored is recorded, so the deflated Sharpe is
computed against the number of things actually tried rather than against one.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


@dataclass(frozen=True)
class Layout:
    root: Path

    @property
    def features(self) -> Path: return self.root / "features"
    @property
    def models(self) -> Path: return self.root / "models"
    @property
    def scorecard(self) -> Path: return self.root / "scorecard.jsonl"
    @property
    def trials(self) -> Path: return self.root / "trials.json"

    def model_path(self, through: str) -> Path:
        return self.models / f"model_{through}.json"

    def feature_path(self, day: str) -> Path:
        return self.features / f"{day}.csv.gz"

    def available_days(self) -> list[str]:
        if not self.features.exists():
            return []
        days = sorted(p.name.split(".")[0] for p in self.features.glob("*.csv.gz"))
        return [d for d in days if len(d) == 8 and d.isdigit()]

    def model_trained_before(self, day: str) -> Path | None:
        """The newest model whose training data ends strictly before `day`.

        Strictly before is the whole point: a model that saw `day` cannot be used
        to make an out-of-sample prediction about it.
        """
        if not self.models.exists():
            return None
        best: tuple[str, Path] | None = None
        for p in self.models.glob("model_*.json"):
            through = p.stem.removeprefix("model_")
            if len(through) == 8 and through.isdigit() and through < day:
                if best is None or through > best[0]:
                    best = (through, p)
        return None if best is None else best[1]


def config_id(config: dict) -> str:
    """Stable id for a configuration, used to count how many have been tried."""
    blob = json.dumps(config, sort_keys=True)
    return hashlib.sha256(blob.encode()).hexdigest()[:12]


def register_trial(layout: Layout, config: dict) -> int:
    """Record a configuration and return how many distinct ones exist so far.

    Called on every scoring run, so tweaking the horizon or the feature set to get
    a better number increments the count that the deflated Sharpe divides by. It is
    deliberately hard to game without editing the file by hand.
    """
    layout.root.mkdir(parents=True, exist_ok=True)
    cid = config_id(config)
    data = {}
    if layout.trials.exists():
        try:
            data = json.loads(layout.trials.read_text())
        except json.JSONDecodeError:
            data = {}
    if cid not in data:
        data[cid] = {"config": config,
                     "first_seen": datetime.now(timezone.utc).isoformat(timespec="seconds")}
        layout.trials.write_text(json.dumps(data, indent=2, sort_keys=True))
    return len(data)


def append_score(layout: Layout, record: dict) -> None:
    layout.root.mkdir(parents=True, exist_ok=True)
    with layout.scorecard.open("a") as fh:
        fh.write(json.dumps(record, sort_keys=True) + "\n")


def read_scores(layout: Layout) -> list[dict]:
    if not layout.scorecard.exists():
        return []
    out = []
    for line in layout.scorecard.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def already_scored(layout: Layout, day: str, cid: str) -> bool:
    return any(r.get("date") == day and r.get("config_id") == cid
               for r in read_scores(layout))
