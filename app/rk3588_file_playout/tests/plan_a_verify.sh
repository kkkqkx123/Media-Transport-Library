#!/bin/bash
# Plan A: offline replay of RX-captured ST22 packets via mpi_dec_test.
#
# Usage:
#   ./tests/plan_a_verify.sh /tmp/rx_capture.h264 [width] [height] [live_decoded]
#
# Example (after 60s TX+RX capture):
#   ./tests/plan_a_verify.sh /tmp/rx_capture.h264 1920 1080 271

set -euo pipefail

DUMP="${1:?usage: $0 <rx_capture.h264> [width] [height] [live_decoded]}"
W="${2:-1920}"
H="${3:-1080}"
LIVE_DEC="${4:-0}"
OUT="${DUMP%.h264}_replay.yuv"
PKT="$(wc -l < "${DUMP%.h264}.idx" 2>/dev/null || echo 0)"

if ! command -v mpi_dec_test >/dev/null 2>&1; then
    echo "ERROR: mpi_dec_test not in PATH"
    exit 1
fi

if [[ ! -f "$DUMP" ]]; then
    echo "ERROR: dump not found: $DUMP"
    exit 1
fi

echo "=== Plan A: offline MPP replay ==="
echo "  input:  $DUMP ($(stat -c%s "$DUMP") bytes)"
echo "  idx:    ${DUMP%.h264}.idx (packets=$PKT)"
echo "  output: $OUT"
echo ""

rm -f "$OUT"
# -n 0 = decode until EOF
mpi_dec_test -t 7 -i "$DUMP" -n 0 -o "$OUT"

if [[ ! -f "$OUT" ]]; then
    echo "ERROR: mpi_dec_test did not create $OUT"
    exit 1
fi

BYTES=$(stat -c%s "$OUT")
FRAMES=$(python3 -c "print(int($BYTES / ($W * $H * 3 // 2)))")
YIELD="0"
if [[ "$PKT" -gt 0 ]]; then
    YIELD=$(python3 -c "print(f'{$FRAMES / $PKT * 100:.1f}')")
fi

echo ""
echo "=== Results ==="
echo "  packets (idx):     $PKT"
echo "  replay frames:     $FRAMES"
echo "  replay yield:      ${YIELD}%  (frames / ST22 packets)"
if [[ "$LIVE_DEC" -gt 0 ]]; then
    echo "  live rx decoded:   $LIVE_DEC"
    LIVE_Y=$(python3 -c "print(f'{$LIVE_DEC / $PKT * 100:.1f}')" 2>/dev/null || echo "?")
    echo "  live yield:        ${LIVE_Y}%"
fi
echo ""

if [[ "$PKT" -gt 0 ]] && python3 -c "exit(0 if $FRAMES / $PKT >= 0.8 else 1)"; then
    echo "PASS: offline replay >= 80% -> problem is live integration/timing"
    exit 0
elif [[ "$PKT" -gt 0 ]]; then
    echo "FAIL: offline replay < 80% -> ST22 packet content/boundary differs from file AU"
    exit 1
else
    echo "WARN: no .idx file; compare replay frames manually"
    exit 0
fi
