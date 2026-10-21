#!/bin/bash
# Single-board loopback: fork mp4_dist_rx then mp4_dist_tx (+2s delay).
#
# Usage:
#   ./tests/mp4_dist_both.sh [seconds] [extra args...]
#
# Example:
#   ./tests/mp4_dist_both.sh 120 --mp4 ../InTheGrey.mp4 --width 1920 --height 1080 --fps 23.976
#
# Env: same as run_single_board_plan_a.sh (MP4_DIST_AU_TRACE, MP4_DIST_DUMP_RX, etc.)

set -euo pipefail

CD="$(cd "$(dirname "$0")/.." && pwd)"
cd "$CD"

SEC="${1:-60}"
shift || true

[ -x ./build/mp4_dist_rx ] || { echo "missing build/mp4_dist_rx — ninja -C build"; exit 1; }
[ -x ./build/mp4_dist_tx ] || { echo "missing build/mp4_dist_tx — ninja -C build"; exit 1; }

RX_PID=""
TX_PID=""
cleanup() {
    [ -n "$TX_PID" ] && kill "$TX_PID" 2>/dev/null || true
    [ -n "$RX_PID" ] && kill "$RX_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "=== mp4_dist_both: RX ${SEC}s, TX starts in 2s ==="
./build/mp4_dist_rx "$SEC" "$@" &
RX_PID=$!

sleep 2
./build/mp4_dist_tx "$SEC" "$@" &
TX_PID=$!

RX_ST=0
TX_ST=0
wait "$RX_PID" && RX_ST=0 || RX_ST=$?
wait "$TX_PID" && TX_ST=0 || TX_ST=$?

if [ "$RX_ST" -eq 0 ] && [ "$TX_ST" -eq 0 ]; then
    echo "=== BOTH PASS (rx=$RX_ST tx=$TX_ST) ==="
    exit 0
fi
echo "=== BOTH FAIL (rx=$RX_ST tx=$TX_ST) ==="
exit 1
