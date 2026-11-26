#!/usr/bin/env bash
# tiramisu/tools/launch-with-autofix.sh
# End-to-end host wiring: transfer a game, launch it on the Series S, pull the
# real tiramisu.log, apply declared fixes and retry — until it boots or a
# terminal/named stopping point is reached.
#
# This drives the existing Device Portal client (tools/console.py) and the
# offline engine (titlebuild/autofix/autopilot). It deliberately does NOT
# modify the host C++ sources: the loop is controllable from the build/control
# machine and every decision is recorded under <game>/.tiramisu/.
#
# Usage:
#   XBOX_IP=... XBOX_PASSWORD=... tools/launch-with-autofix.sh <game_dir> [attempts]
#   tools/launch-with-autofix.sh --dry-run <game_dir> <log_dir> [attempts]
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"

if [[ "${1:-}" == "--dry-run" ]]; then
  shift
  game="$1"; logs="$2"; shift 2
  exec python3 tools/autopilot.py "$game" --dry-run "$logs" \
    --max-attempts "${1:-5}"
fi

game="${1:-}"
[[ -n "$game" && -d "$game" ]] || {
  echo "usage: launch-with-autofix.sh [--dry-run] <game_dir> [attempts]" >&2
  exit 2
}
attempts="${2:-5}"
game_abs="$(cd "$game" && pwd)"
mkdir -p logs

# Build the measured manifest before the first launch.
python3 tools/titlebuild.py "$game_abs" || true

launch_cmd="python3 tools/console.py launch --game '$game_abs'"
log_cmd="python3 tools/console.py logs >/dev/null 2>&1 && \
  log=\$(ls -t logs/*-tiramisu.log 2>/dev/null | head -1) && \
  { [[ -n \"\$log\" ]] && cat \"\$log\" || echo 'no tiramisu.log on console'; }"

exec python3 tools/autopilot.py "$game_abs" \
  --launch-cmd "$launch_cmd" --log-cmd "$log_cmd" \
  --max-attempts "$attempts"
