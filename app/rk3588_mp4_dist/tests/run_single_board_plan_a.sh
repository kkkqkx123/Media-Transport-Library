#!/bin/bash
# Single-board Plan A: trace + dump + compare + offline MPP replay.
#
# Usage:
#   ./tests/run_single_board_plan_a.sh [seconds] [mp4_path] [log_path]
#
# Env (optional):
#   MP4_DIST_MPP_AGGRESSIVE_PULL=1  — idle pull after each AU + EOS flush
#   MP4_DIST_MPP_WHOLE_AU=1         — whole AU feed (A/B only; default is legacy)
#
# Uses mp4_dist_both.sh (RX first, TX +2s). Defaults match mp4_dist_config:
#   TX: enP3p49s0 @ 192.168.80.213 -> RX: eth0 @ 192.168.80.212

set -euo pipefail

SEC="${1:-120}"
MP4="${2:-../InTheGrey.mp4}"
LOG="${3:-/tmp/run1.log}"
BASE="/tmp/run1"
CD="$(cd "$(dirname "$0")/.." && pwd)"

cd "$CD"
[ -x ./build/mp4_dist_rx ] || { echo "run: ninja -C build first"; exit 1; }
[ -x ./build/mp4_dist_tx ] || { echo "run: ninja -C build first"; exit 1; }
chmod +x tests/mp4_dist_both.sh 2>/dev/null || true

echo "=== clean old trace/dump (avoid NUL/corrupt csv) ==="
rm -f "${BASE}.tx.csv" "${BASE}.tx_wire.csv" \
      "${BASE}.rx_mtl.csv" "${BASE}.rx_br.csv" \
      /tmp/rx_capture.h264 /tmp/rx_capture.idx \
      /tmp/rx_capture_replay.yuv

export MP4_DIST_AU_TRACE="$BASE"
export MP4_DIST_DUMP_RX=/tmp/rx_capture.h264
export MP4_DIST_DUMP_RX_IDX=/tmp/rx_capture.idx

echo "=== env ==="
echo "  test_sec=$SEC  log=$LOG"
echo "  MP4_DIST_MPP_WHOLE_AU=${MP4_DIST_MPP_WHOLE_AU:-<unset>}"
echo "  MP4_DIST_MPP_AGGRESSIVE_PULL=${MP4_DIST_MPP_AGGRESSIVE_PULL:-<unset>}"

echo "=== mp4_dist_both ${SEC}s ==="
./tests/mp4_dist_both.sh "$SEC" --mp4 "$MP4" \
  --width 1920 --height 1080 --fps 23.976 2>&1 | tee "$LOG"

echo ""
echo "=== live decode summary ==="
grep -E 'mpp_dec:|mpp_dec stats|rx_pipe:' "$LOG" || true

echo ""
echo "=== trace files ==="
ls -l "${BASE}"*.csv /tmp/rx_capture.h264 2>/dev/null || true
for f in "${BASE}.tx.csv" "${BASE}.tx_wire.csv" "${BASE}.rx_mtl.csv" "${BASE}.rx_br.csv"; do
  if [ -f "$f" ]; then
    echo -n "$f: "
    head -1 "$f"
  else
    echo "MISSING: $f"
  fi
done

echo ""
echo "=== compare_au_trace ==="
python3 tests/compare_au_trace.py "$BASE"

if [ -f /tmp/rx_capture.h264 ] && command -v mpi_dec_test >/dev/null 2>&1; then
  echo ""
  echo "=== offline mpi_dec_test replay ==="
  mpi_dec_test -t 7 -i /tmp/rx_capture.h264 -n 0 -o /tmp/rx_capture_replay.yuv
  PKTS=$(wc -l < /tmp/rx_capture.idx)
  FR=$(python3 -c "import os; s=os.path.getsize('/tmp/rx_capture_replay.yuv'); print(int(s/(1920*1080*3//2)))")
  echo "capture packets=$PKTS  offline_frames=$FR  yield=$(python3 -c "print(f'{$FR/$PKTS*100:.1f}%')")"
  if grep -q 'rx_pipe:' "$LOG"; then
    python3 - <<'PY' "$LOG" "$PKTS" "$FR"
import re, sys
log, pkts, offline = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
m = re.search(r'rx_pipe: (\d+) pkt, (\d+) dec', open(log).read())
if m:
    rx_pkt, rx_dec = int(m.group(1)), int(m.group(2))
    print(f"live dec/pkt: {rx_dec}/{rx_pkt} = {100*rx_dec/rx_pkt:.1f}%  "
          f"(offline ref {offline}/{pkts} = {100*offline/pkts:.1f}%)")
PY
  fi
fi

echo ""
echo "=== check_pts_idx ==="
if [ -f /tmp/rx_capture.idx ]; then
  python3 tests/check_pts_idx.py /tmp/rx_capture.idx --fps 23.976 --max-zero-pct 0 || true
else
  echo "SKIP: /tmp/rx_capture.idx missing"
fi
