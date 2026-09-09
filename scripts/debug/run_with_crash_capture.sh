#!/bin/sh
# Run PearPC with timestamped logs and crash dumps.
# Usage: scripts/debug/run_with_crash_capture.sh [config-file] [ppc options...]

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CONFIG=${1:-ppccfg.osx}
if [ "$#" -gt 0 ]; then
    shift
fi

STAMP=$(date '+%Y%m%d-%H%M%S')-$$
OUT=${PEARPC_CRASH_DIR:-"$ROOT/crash-captures"}/$STAMP
mkdir -p "$OUT" || exit 1

if [ -f "$CONFIG" ]; then
    cp "$CONFIG" "$OUT/config.input"
fi

LOG="$OUT/console.log"
printf 'PearPC crash capture: %s\n' "$OUT"
printf 'Logs: %s\n' "$LOG"

"$ROOT/src/ppc" \
    --memdump-file="$OUT/guest-memory.bin" \
    --framebuffer-dump-file="$OUT/framebuffer.bin" \
    "$CONFIG" "$@" >"$LOG" 2>&1
STATUS=$?

printf '\nPearPC exited with status %d\n' "$STATUS" >>"$LOG"
printf 'PearPC exited with status %d; capture saved in %s\n' "$STATUS" "$OUT"
exit "$STATUS"
