#!/bin/bash
# Per-phase automated verification (board-only; RK3588 cross-compile).
#
# Usage:
#   ./tests/verify_phase.sh P0 [mp4_path]
#   ./tests/verify_phase.sh P1 [seconds] [mp4_path]
#   ./tests/verify_phase.sh P2 [seconds] [mp4_path]
#   ./tests/verify_phase.sh P4 [seconds] [mp4_path]
#
# L2 board runs pair file_playout_rx with the live_ingest TX sender
#   FILE_PLAYOUT_VERIFY_LOG    — log path for L2 run

set -euo pipefail

PHASE="${1:?usage: $0 P0|P1|P2|P3|P4 [args...]}"
shift || true

CD="$(cd "$(dirname "$0")/.." && pwd)"
cd "$CD"

MP4="${MP4:-../InTheGrey.mp4}"
SEC="${FILE_PLAYOUT_VERIFY_SEC:-60}"
LOG="${FILE_PLAYOUT_VERIFY_LOG:-/tmp/verify_phase.log}"
IDX="/tmp/rx_capture.idx"
BASE="/tmp/verify_phase"

fail() { echo "FAIL: $*"; exit 1; }
pass() { echo "PASS: $*"; }

RX_PID=""; TX_PID=""
# 单板回环同 IP：历史硬编码 .213/.212 会导致 RX 收不到包，统一经 L2_IP 覆盖。
L2_IP="${L2_IP:-192.168.80.202}"
cleanup_l2() {
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
trap cleanup_l2 INT TERM EXIT
preflight_l2() {
  pgrep -f MtlManager >/dev/null || fail "MtlManager 未运行（先起 MtlManager 再跑 L2）"
  [ -c /dev/video0 ] || fail "无 /dev/video0（L2 需 HDMI 真实源）"
  pkill -INT live_ingest 2>/dev/null || true
  pkill -INT file_playout_rx 2>/dev/null || true
  sleep 1
  pkill -9 live_ingest 2>/dev/null || true
  pkill -9 file_playout_rx 2>/dev/null || true
  sleep 1
}

l1_p0() {
    echo "=== P0: toolchain + cross-package binaries ==="
    [ -x ./build/file_playout_rx ] || fail "build/file_playout_rx missing — run: meson setup build && ninja -C build"
    [ -x ../rk3588_live_tx/build/live_ingest ] || fail "live_ingest missing — build the encode package first"
    [ -f tests/run_single_board_plan_a.sh ] || fail "run_single_board_plan_a.sh missing"
    [ -f tests/check_pts_idx.py ] || fail "check_pts_idx.py missing"
    [ -f IMPLEMENTATION_PLAN.md ] || fail "IMPLEMENTATION_PLAN.md missing"
    chmod +x tests/run_single_board_plan_a.sh tests/verify_phase.sh \
        tests/check_pts_idx.py tests/check_playout_log.py tests/check_drm_log.py 2>/dev/null || true
    echo "  tools: OK"

    if [ -n "${1:-}" ] && [ -f "$1" ]; then
        echo "=== P0 L1: probe ==="
        ./build/file_playout_rx 1 --probe-mp4 "$1" 2>&1 | tee /tmp/probe_out.txt || true
        grep -q 'probe:' /tmp/probe_out.txt || fail "probe output missing"
        echo "  probe: OK"
    else
        echo "  probe: SKIP (no mp4: ${1:-<unset>})"
    fi
}

l1_p1() {
    echo "=== P1 L1: media_clk unit test ==="
    if [ -x ./build/test_media_clk ]; then
        ./build/test_media_clk || fail "test_media_clk"
        echo "  test_media_clk: OK"
    else
        fail "test_media_clk missing — ninja -C build"
    fi
}

l2_run() {
    local sec="$1" mp4="$2" fps="${3:-60.0}"
    echo "=== L2: RX + live_ingest sender ${sec}s (fps=$fps, ip=$L2_IP, HDMI required) ==="
    preflight_l2
    rm -f "${BASE}"*.csv "$IDX" /tmp/rx_capture.h264
    export FILE_PLAYOUT_AU_TRACE="$BASE"
    export FILE_PLAYOUT_DUMP_RX=/tmp/rx_capture.h264
    export FILE_PLAYOUT_DUMP_RX_IDX="$IDX"

    ./build/file_playout_rx "$sec" --udp 15000 --sip "$L2_IP" \
        --width 1920 --height 1080 --fps "$fps" 2>&1 | tee "$LOG" &
    RX_PID=$!
    sleep 2
    kill -0 "$RX_PID" 2>/dev/null || fail "RX 启动失败（查 MtlManager 与 $LOG）"
    ../rk3588_live_tx/build/live_ingest "$sec" --udp 15000 \
        --dip "$L2_IP" 2>&1 | tee -a "$LOG" &
    TX_PID=$!
    wait "$RX_PID" || true
    wait "$TX_PID" || true
    RX_PID=""; TX_PID=""

    grep -E 'rx_pipe:|mpp_dec stats|mtl_rx_bridge:' "$LOG" || true
}

l2_check_pts() {
    local fps="${1:-60.0}"
    [ -f "$IDX" ] || fail "missing $IDX"
    python3 tests/check_pts_idx.py "$IDX" --fps "$fps" --max-zero-pct 0
}

l2_check_p2() {
    python3 tests/check_playout_log.py "$LOG"
}

l2_check_p4() {
    local fps="${1:-60.0}"
    if [ -x ./build/test_drm_mode ]; then
        ./build/test_drm_mode || fail "test_drm_mode"
    fi
    python3 tests/check_drm_log.py "$LOG" --fps "$fps"
}

case "$PHASE" in
P0)
    MP4_ARG="${1:-$MP4}"
    l1_p0 "$MP4_ARG"
    pass "P0"
    ;;
P1)
    SEC="${1:-$SEC}"
    MP4="${2:-$MP4}"
    FPS="${FILE_PLAYOUT_VERIFY_FPS:-60.0}"
    l1_p1
    if [ "${FILE_PLAYOUT_SKIP_L2:-0}" != "1" ]; then
        l2_run "$SEC" "$MP4" "$FPS"
        l2_check_pts "$FPS"
    else
        echo "L2: SKIP (FILE_PLAYOUT_SKIP_L2=1)"
    fi
    pass "P1"
    ;;
P2|P3)
    SEC="${1:-$SEC}"
    MP4="${2:-$MP4}"
    FPS="${FILE_PLAYOUT_VERIFY_FPS:-60.0}"
    l1_p1
    if [ "${FILE_PLAYOUT_SKIP_L2:-0}" != "1" ]; then
        l2_run "$SEC" "$MP4" "$FPS"
        l2_check_pts "$FPS"
        l2_check_p2
    else
        echo "L2: SKIP (FILE_PLAYOUT_SKIP_L2=1)"
    fi
    pass "$PHASE"
    ;;
P4)
    SEC="${1:-$SEC}"
    MP4="${2:-$MP4}"
    FPS="${FILE_PLAYOUT_VERIFY_FPS:-60.0}"
    l1_p1
    if [ -x ./build/test_drm_mode ]; then
        ./build/test_drm_mode || fail "test_drm_mode"
    else
        fail "test_drm_mode missing — ninja -C build"
    fi
    if [ "${FILE_PLAYOUT_SKIP_L2:-0}" != "1" ]; then
        l2_run "$SEC" "$MP4" "$FPS"
        l2_check_p4 "$FPS"
    else
        echo "L2: SKIP (FILE_PLAYOUT_SKIP_L2=1)"
    fi
    pass "P4"
    ;;
*)
    fail "unknown phase $PHASE (supported: P0, P1, P2, P3, P4)"
    ;;
esac
