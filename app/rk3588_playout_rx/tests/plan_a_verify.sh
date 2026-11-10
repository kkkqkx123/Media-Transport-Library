#!/bin/bash
# Plan A: offline replay of RX-captured ST22 packets.
#
# Usage:
#   ./tests/plan_a_verify.sh /tmp/rx_capture.h264 [width] [height] [live_decoded] [codec]
#
# Codec is auto-detected from the dump suffix (.h265/.hevc = h265, else h264);
# pass [codec] or CODEC=h264|h265 to override. H264 replays via mpi_dec_test
# (type 7); H265 replays via the ffmpeg native hevc decoder (no MPP needed).
# A raw Annex B classifier check runs on the dump in both cases.

set -euo pipefail

DUMP="${1:?usage: $0 <dump.h264|dump.h265> [width] [height] [live_decoded] [codec]}"
W="${2:-1920}"
H="${3:-1080}"
LIVE_DEC="${4:-0}"
CODEC_ARG="${5:-${CODEC:-auto}}"
TDIR="$(cd "$(dirname "$0")" && pwd)"

if [[ ! -f "$DUMP" ]]; then
    echo "ERROR: dump not found: $DUMP"
    exit 1
fi

case "$CODEC_ARG" in
  auto)
    case "$DUMP" in
      *.h265|*.hevc) CODEC="h265" ;;
      *) CODEC="h264" ;;
    esac
    ;;
  h264|h265) CODEC="$CODEC_ARG" ;;
  *) echo "ERROR: bad codec $CODEC_ARG (want h264|h265|auto)"; exit 1 ;;
esac

OUT="${DUMP%.*}_replay.yuv"
IDX="${DUMP%.*}.idx"
PKT="$(wc -l < "$IDX" 2>/dev/null || echo 0)"

echo "=== Plan A: offline replay ($CODEC) ==="
echo "  input:  $DUMP ($(stat -c%s "$DUMP") bytes)"
echo "  idx:    $IDX (packets=$PKT)"
echo "  output: $OUT"
echo ""

echo "--- annexb codec check ---"
python3 "$TDIR/check_codec_annexb.py" "$DUMP" --expect "$CODEC" || exit 1
echo ""

rm -f "$OUT"
if [[ "$CODEC" == "h265" ]]; then
    command -v ffmpeg >/dev/null 2>&1 || { echo "ERROR: ffmpeg not in PATH"; exit 1; }
    ffmpeg -v error -f hevc -i "$DUMP" -pix_fmt yuv420p -f rawvideo -y "$OUT" \
        || { echo "ERROR: ffmpeg hevc replay failed"; exit 1; }
else
    if ! command -v mpi_dec_test >/dev/null 2>&1; then
        echo "ERROR: mpi_dec_test not in PATH"
        exit 1
    fi
    # -n 0 = decode until EOF
    mpi_dec_test -t 7 -i "$DUMP" -n 0 -o "$OUT"
fi

if [[ ! -f "$OUT" ]]; then
    echo "ERROR: replay did not create $OUT"
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
