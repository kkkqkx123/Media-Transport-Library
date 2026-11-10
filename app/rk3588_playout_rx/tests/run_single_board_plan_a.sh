#!/bin/bash
# Single-board Plan A (decode role): RX capture + dump + compare + offline MPP replay.
#
# Sender is the encode package (live_ingest TX, HDMI capture). Start RX first,
# then the sender with a matching UDP port / fps / resolution. HDMI signal is
# required; without it live_ingest retries and RX captures nothing.
#
# Usage:
#   ./tests/run_single_board_plan_a.sh [seconds] [rx_args...]
#
# Env (optional):
#   CODEC=h264|h265 (default h264) — passed as --codec to TX and RX;
#     h265 needs an HEVC-capable MPP stack on both ends.
#   FILE_PLAYOUT_MPP_AGGRESSIVE_PULL=1  — idle pull after each AU + EOS flush
#   FILE_PLAYOUT_MPP_WHOLE_AU=1         — whole AU feed (A/B only; default is legacy)

set -euo pipefail

SEC="${1:-120}"
shift || true
CODEC="${CODEC:-h264}"
case "$CODEC" in h264|h265) ;; *) echo "bad CODEC=$CODEC (want h264|h265)"; exit 1 ;; esac
if [ "$CODEC" = "h265" ]; then EXT="h265"; else EXT="h264"; fi
DUMP="/tmp/rx_capture.$EXT"
DUMP_IDX="/tmp/rx_capture.idx"
DUMP_YUV="/tmp/rx_capture_replay.yuv"
LOG="/tmp/run1.log"
RX_LOG="/tmp/run1.rx.log"
BASE="/tmp/run1"
CD="$(cd "$(dirname "$0")/.." && pwd)"
LIVE_INGEST="$CD/../rk3588_live_tx/build/live_ingest"
# 单板回环同 IP：历史硬编码 .213/.212 会导致 RX 收不到包，统一经 L2_IP 覆盖。
L2_IP="${L2_IP:-192.168.80.202}"
RX_PID=""; TX_PID=""
cleanup_plan_a() {
  trap - INT TERM EXIT
  for p in ${RX_PID:-} ${TX_PID:-}; do
    [ -n "$p" ] && kill -0 "$p" 2>/dev/null || continue
    kill -INT "$p" 2>/dev/null || true
  done
  sleep 5
  for p in ${RX_PID:-} ${TX_PID:-}; do
    [ -n "$p" ] && kill -0 "$p" 2>/dev/null || continue
    kill -9 "$p" 2>/dev/null || true
  done
}
trap cleanup_plan_a INT TERM EXIT

cd "$CD"
[ -x ./build/file_playout_rx ] || { echo "run: ninja -C build first"; exit 1; }
[ -x "$LIVE_INGEST" ] || { echo "run: build rk3588_live_tx first (encode sender)"; exit 1; }
pgrep -f MtlManager >/dev/null || { echo "MtlManager 未运行（先起 MtlManager 再跑）"; exit 1; }
pkill -INT live_ingest 2>/dev/null || true
pkill -INT file_playout_rx 2>/dev/null || true
sleep 1
pkill -9 live_ingest 2>/dev/null || true
pkill -9 file_playout_rx 2>/dev/null || true
sleep 1

echo "=== clean old trace/dump (avoid NUL/corrupt csv) ==="
rm -f "${BASE}.rx_mtl.csv" "${BASE}.rx_br.csv" \
      /tmp/rx_capture.h264 /tmp/rx_capture.h265 /tmp/rx_capture.idx \
      /tmp/rx_capture_replay.yuv

export FILE_PLAYOUT_AU_TRACE="$BASE"
export FILE_PLAYOUT_DUMP_RX="$DUMP"
export FILE_PLAYOUT_DUMP_RX_IDX="$DUMP_IDX"

echo "=== env ==="
echo "  test_sec=$SEC  log=$LOG"
echo "  FILE_PLAYOUT_MPP_WHOLE_AU=${FILE_PLAYOUT_MPP_WHOLE_AU:-<unset>}"
echo "  FILE_PLAYOUT_MPP_AGGRESSIVE_PULL=${FILE_PLAYOUT_MPP_AGGRESSIVE_PULL:-<unset>}"

echo "=== RX ${SEC}s (sender: live_ingest, ip=$L2_IP, HDMI required) ==="
rm -f "$LOG" "$RX_LOG"
./build/file_playout_rx "$SEC" --udp 15000 --sip "$L2_IP" \
  --width 1920 --height 1080 --fps 60.0 --codec "$CODEC" "$@" >"$RX_LOG" 2>&1 &
RX_PID=$!

sleep 2
kill -0 "$RX_PID" 2>/dev/null || { tail -n 20 "$RX_LOG"; echo "RX 启动失败"; exit 1; }
"$LIVE_INGEST" "$SEC" --udp 15000 --dip "$L2_IP" --codec "$CODEC" 2>&1 | tee "$LOG" &
TX_PID=$!

wait "$RX_PID" && RX_ST=0 || RX_ST=$?
wait "$TX_PID" && TX_ST=0 || TX_ST=$?
RX_PID=""; TX_PID=""
cat "$RX_LOG" >>"$LOG"
echo "=== done rx=$RX_ST tx=$TX_ST (rx log: $RX_LOG, merged: $LOG) ==="

echo ""
echo "=== live decode summary ==="
grep -E 'mpp_dec:|mpp_dec stats|rx_pipe:' "$LOG" || true

echo ""
echo "=== trace files ==="
ls -l "${BASE}"*.csv "$DUMP" 2>/dev/null || true
for f in "${BASE}.rx_mtl.csv" "${BASE}.rx_br.csv"; do
  if [ -f "$f" ]; then
    echo -n "$f: "
    head -1 "$f"
  else
    echo "MISSING: $f"
  fi
done

echo ""
echo "=== offline replay ($CODEC) + annexb codec check ==="
python3 tests/check_codec_annexb.py "$DUMP" --expect "$CODEC" || true
if [ -f "$DUMP" ]; then
  if [ "$CODEC" = "h265" ]; then
    if command -v ffmpeg >/dev/null 2>&1; then
      ffmpeg -v error -f hevc -i "$DUMP" -pix_fmt yuv420p -f rawvideo -y "$DUMP_YUV"
    else
      echo "SKIP: no ffmpeg for hevc replay"
    fi
  elif command -v mpi_dec_test >/dev/null 2>&1; then
    mpi_dec_test -t 7 -i "$DUMP" -n 0 -o "$DUMP_YUV"
  else
    echo "SKIP: no mpi_dec_test for h264 replay"
  fi
fi
if [ -f "$DUMP_YUV" ] && [ -f "$DUMP_IDX" ]; then
  PKTS=$(wc -l < "$DUMP_IDX")
  FR=$(python3 -c "import os; s=os.path.getsize('$DUMP_YUV'); print(int(s/(1920*1080*3//2)))")
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
  python3 tests/check_pts_idx.py /tmp/rx_capture.idx --fps 60.0 --max-zero-pct 0 || true
else
  echo "SKIP: /tmp/rx_capture.idx missing"
fi
