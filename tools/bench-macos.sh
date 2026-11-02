#!/bin/bash
# Repeatable gameplay benchmark on macOS: launches the app with crackdown_benchmark, which continues the saved game
# with a virtual pad, stands still, records frame times and quits. Relaunches attempts that never reach gameplay.
#
# Usage: tools/bench-macos.sh [-n runs] [-l label] [-r frame rate] [-e VAR=value]... [-- extra game arguments]
# The frame rate defaults to 0, unlimited. Give it with -r and not as an extra argument: a flag given twice makes the
# SDK's parser drop every launch flag.
# Environment: CRACKDOWN_APP (the .app), CRACKDOWN_LOGS (its logs folder, where results are written).
set -euo pipefail

runs=3
label=bench
rate=0
envs=()
while [ $# -gt 0 ]; do
  case "$1" in
    -n) runs=$2; shift 2 ;;
    -l) label=$2; shift 2 ;;
    -r) rate=$2; shift 2 ;;
    -e) envs+=(--env "$2"); shift 2 ;;
    --) shift; break ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

app=${CRACKDOWN_APP:?set CRACKDOWN_APP to the app bundle}
logs=${CRACKDOWN_LOGS:?set CRACKDOWN_LOGS to the logs folder}
max_attempts=$((runs * 3))
results=()
attempt=0
while [ ${#results[@]} -lt "$runs" ] && [ $attempt -lt $max_attempts ]; do
  attempt=$((attempt + 1))
  pkill -x crackdown 2>/dev/null || true
  sleep 2
  marker=$(mktemp)
  open -W -n ${envs[@]+"${envs[@]}"} -a "$app" --args --crackdown_skip_launcher=true --crackdown_frame_rate="$rate" \
    --crackdown_benchmark=true --crackdown_benchmark_label="$label" "$@" || true
  result=$(find "$logs" -name 'benchmark-*.json' -newer "$marker" -print | sort | tail -1)
  rm -f "$marker"
  if [ -z "$result" ]; then
    echo "attempt $attempt: no result (crashed or closed)"
  elif grep -q '"status":"ok"' "$result"; then
    echo "attempt $attempt: $(cat "$result")"
    results+=("$result")
  else
    echo "attempt $attempt: $(cat "$result") - relaunching"
  fi
done

[ ${#results[@]} -gt 0 ] || { echo "no successful runs"; exit 1; }
python3 - "${results[@]}" <<'EOF'
import json, statistics, sys
rows = [json.load(open(p)) for p in sys.argv[1:]]
def summary(key):
    values = [r[key] for r in rows]
    return f"{statistics.median(values):6.2f} (min {min(values):.2f}, max {max(values):.2f})"
print(f"{rows[0]['label']}: {len(rows)} runs")
for key in ("average_fps", "one_percent_low_fps", "median_frame_ms", "p99_frame_ms"):
    print(f"  {key:20s} {summary(key)}")
EOF
