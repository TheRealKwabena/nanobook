#!/usr/bin/env python3
"""Stream one day of IEX DEEP through iex_replay and store the feature table.

The point of this script is what it does NOT do: it never lands the capture on
disk. A single day of IEX DEEP is 11-12 GB gzipped, and the pipeline only needs
the derived features, which are a few megabytes. So the download, the
decompression and the decode are one pipeline:

    curl -sL URL | gunzip | iex_replay --pcap - ... | gzip > features.csv.gz

Peak disk usage is the output. Peak memory is one stream buffer plus one ladder
per watched symbol.

IEX publishes HIST free on a T+1 basis; using it means agreeing to the IEX
Historical Data Terms of Use.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import urllib.request
from datetime import date, datetime, timedelta
from pathlib import Path

HIST_API = "https://iextrading.com/api/1.0/hist"
REPO = Path(__file__).resolve().parent.parent
IEX_REPLAY = REPO / "build" / "bin" / "iex_replay"


def hist_entries(day: str) -> list[dict]:
    """Available feeds for a YYYYMMDD date."""
    with urllib.request.urlopen(f"{HIST_API}?date={day}", timeout=60) as r:
        payload = json.load(r)
    # The API returns a bare list for a dated query and a dict for the full index.
    if isinstance(payload, dict):
        return payload.get(day, [])
    return payload


def pick_feed(entries: list[dict], feed: str) -> dict | None:
    for e in entries:
        if e.get("feed") == feed:
            return e
    return None


def latest_available(max_lookback: int = 10) -> str | None:
    """Walk back from yesterday until a DEEP file exists (skips weekends/holidays)."""
    day = date.today() - timedelta(days=1)
    for _ in range(max_lookback):
        stamp = day.strftime("%Y%m%d")
        try:
            if pick_feed(hist_entries(stamp), "DEEP"):
                return stamp
        except Exception as exc:  # noqa: BLE001 - network/HTTP shape varies
            print(f"  {stamp}: lookup failed ({exc})", file=sys.stderr)
        day -= timedelta(days=1)
    return None


def run(day: str, symbols: str, out_dir: Path, sample_ms: int,
        max_packets: int, feed: str) -> int:
    entry = pick_feed(hist_entries(day), feed)
    if entry is None:
        print(f"no {feed} file published for {day}", file=sys.stderr)
        return 2

    size_gb = int(entry["size"]) / 1e9
    out_dir.mkdir(parents=True, exist_ok=True)
    out_csv = out_dir / f"{day}.csv.gz"
    log_path = out_dir / f"{day}.log"

    print(f"date       {day}")
    print(f"feed       {feed} v{entry['version']}  ({size_gb:.2f} GB gzipped)")
    print(f"symbols    {symbols}")
    print(f"grid       {sample_ms} ms")
    print(f"output     {out_csv}")
    if max_packets:
        print(f"limit      {max_packets:,} packets (partial day)")
    print("streaming — the capture is never written to disk")

    # Pipe the three stages so nothing buffers a whole file. `curl` writes to
    # gunzip, gunzip to iex_replay, iex_replay's CSV to gzip.
    curl = subprocess.Popen(
        ["curl", "-sL", "--fail", "--retry", "3", "--retry-delay", "5", entry["link"]],
        stdout=subprocess.PIPE,
    )
    gunzip = subprocess.Popen(["gunzip", "-c"], stdin=curl.stdout, stdout=subprocess.PIPE)
    curl.stdout.close()

    cmd = [str(IEX_REPLAY), "--pcap", "-", "--symbols", symbols,
           "--out", "-", "--sample-ms", str(sample_ms), "--progress"]
    if max_packets:
        cmd += ["--max-packets", str(max_packets)]

    with open(log_path, "wb") as log, open(out_csv, "wb") as out:
        gzip_proc = subprocess.Popen(["gzip", "-6"], stdin=subprocess.PIPE, stdout=out)
        replay = subprocess.Popen(cmd, stdin=gunzip.stdout, stdout=gzip_proc.stdin, stderr=log)
        gunzip.stdout.close()
        gzip_proc.stdin.close()
        replay_rc = replay.wait()
        gzip_proc.wait()
    gunzip.wait()
    # curl is killed by SIGPIPE when --max-packets stops the reader early; that is
    # expected and not a failure.
    curl.wait()

    report = log_path.read_text(errors="replace")
    print(report)

    if replay_rc != 0:
        print(f"iex_replay exited {replay_rc} — see {log_path}", file=sys.stderr)
        print("a nonzero exit means the transport reported defects; "
              "features from this run should not be trusted", file=sys.stderr)
        return 1

    rows = out_csv.stat().st_size
    print(f"wrote {out_csv}  ({rows/1e6:.1f} MB gzipped)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--date", default="latest",
                    help="YYYYMMDD, or 'latest' to walk back to the newest published day")
    ap.add_argument("--symbols", default="SPY,AAPL,MSFT,NVDA,TSLA,AMD,QQQ,META",
                    help="comma-separated watchlist")
    ap.add_argument("--out-dir", type=Path, default=REPO / "data" / "features")
    ap.add_argument("--sample-ms", type=int, default=1000,
                    help="feature grid in milliseconds (0 = every book transaction)")
    ap.add_argument("--max-packets", type=int, default=0,
                    help="stop early; useful for a quick test against a real day")
    ap.add_argument("--feed", default="DEEP")
    args = ap.parse_args()

    if not IEX_REPLAY.exists():
        print(f"{IEX_REPLAY} not found — run `make tools` first", file=sys.stderr)
        return 2
    for tool in ("curl", "gunzip", "gzip"):
        if shutil.which(tool) is None:
            print(f"required tool '{tool}' not found on PATH", file=sys.stderr)
            return 2

    day = args.date
    if day == "latest":
        print("resolving the most recent published DEEP file ...")
        day = latest_available()
        if day is None:
            print("could not find a published DEEP file in the last 10 days", file=sys.stderr)
            return 2

    if len(day) != 8 or not day.isdigit():
        print(f"--date must be YYYYMMDD, got {day!r}", file=sys.stderr)
        return 2

    started = datetime.now()
    rc = run(day, args.symbols, args.out_dir, args.sample_ms, args.max_packets, args.feed)
    print(f"elapsed {(datetime.now() - started).total_seconds():.0f}s")
    return rc


if __name__ == "__main__":
    sys.exit(main())
