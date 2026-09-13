#!/usr/bin/env bash
# Wrapper for the launchd job. launchd starts jobs with a minimal environment and
# no PATH to speak of, so the tools the pipeline shells out to are located here
# rather than assumed.
set -uo pipefail
cd "$(dirname "$0")/../.."

export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:$HOME/.local/bin"

echo "════════════════════════════════════════════════════════════════════"
echo "nanobook daily · $(date '+%Y-%m-%d %H:%M:%S %Z')"
echo "════════════════════════════════════════════════════════════════════"

# Don't start a 12 GB download on a connection that cannot finish it, and don't
# fight for bandwidth if a previous run is somehow still going.
if pgrep -qf "scripts/fetch_day.py"; then
    echo "a fetch is already running; skipping this night"
    exit 0
fi

./nanobook daily
rc=$?

echo
echo "exit $rc · finished $(date '+%H:%M:%S')"
# Keep the log from growing without bound: retain the last ~4000 lines.
if [ -f data/daily.log ] && [ "$(wc -l < data/daily.log)" -gt 8000 ]; then
    tail -4000 data/daily.log > data/daily.log.tmp && mv data/daily.log.tmp data/daily.log
fi
exit $rc
