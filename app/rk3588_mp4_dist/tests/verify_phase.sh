#!/bin/bash
# Per-phase automated verification (L1 offline + L2 board short-run).
#
# Usage:
#   ./tests/verify_phase.sh P0 [mp4_path]
#   ./tests/verify_phase.sh P1 [seconds] [mp4_path]
#   ./tests/verify_phase.sh P2 [seconds] [mp4_path]
#   ./tests/verify_phase.sh P4 [seconds] [mp4_path]
#
# Env:
#   MP4_DIST_SKIP_L2=1     — skip board run (L1 only)
#   MP4_DIST_VERIFY_LOG    — log path for L2 run

set -euo pipefail

PHASE="${1:?usage: $0 P0|P1|P2|P3|P4 [args...]}"
shift || true

CD="$(cd "$(dirname "$0")/.." && pwd)"
cd "$CD"

MP4="${MP4:-../InTheGrey.mp4}"
SEC="${MP4_DIST_VERIFY_SEC:-60}"
LOG="${MP4_DIST_VERIFY_LOG:-/tmp/verify_phase.log}"
IDX="/tmp/rx_capture.idx"
BASE="/tmp/verify_phase"

fail() { echo "FAIL: $*"; exit 1; }
pass() { echo "PASS: $*"; }

l1_p0() {
    echo "=== P0 L1: toolchain ==="
    [ -x ./build/mp4_dist_tx ] || fail "build/mp4_dist_tx missing — run: meson setup build && ninja -C build"
    [ -x ./build/mp4_dist_rx ] || fail "build/mp4_dist_rx missing — run: meson setup build && ninja -C build"
    [ -f tests/mp4_dist_both.sh ] || fail "mp4_dist_both.sh missing"
    [ -f tests/run_single_board_plan_a.sh ] || fail "run_single_board_plan_a.sh missing"
    [ -f tests/check_pts_idx.py ] || fail "check_pts_idx.py missing"
    [ -f IMPLEMENTATION_PLAN.md ] || fail "IMPLEMENTATION_PLAN.md missing"
    chmod +x tests/run_single_board_plan_a.sh tests/verify_phase.sh \
        tests/mp4_dist_both.sh \
        tests/check_pts_idx.py tests/check_playout_log.py tests/check_drm_log.py 2>/dev/null || true
    echo "  tools: OK"

    if [ -n "${1:-}" ] && [ -f "$1" ]; then
        echo "=== P0 L1: probe ==="
        ./build/mp4_dist_tx 1 --mp4 "$1" --probe-mp4 "$1" 2>&1 | tee /tmp/probe_out.txt || true
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
    local sec="$1" mp4="$2" fps="${3:-23.976}"
    echo "=== L2: both mode ${sec}s (fps=$fps) ==="
    rm -f "${BASE}"*.csv "$IDX" /tmp/rx_capture.h264
    export MP4_DIST_AU_TRACE="$BASE"
    export MP4_DIST_DUMP_RX=/tmp/rx_capture.h264
    export MP4_DIST_DUMP_RX_IDX="$IDX"

    ./tests/mp4_dist_both.sh "$sec" --mp4 "$mp4" \
        --width 1920 --height 1080 --fps "$fps" 2>&1 | tee "$LOG"

    grep -E 'rx_pipe:|mpp_dec stats|mtl_rx_bridge:' "$LOG" || true
    python3 tests/compare_au_trace.py "$BASE" || true
}

l2_check_pts() {
    local fps="${1:-23.976}"
    [ -f "$IDX" ] || fail "missing $IDX"
    python3 tests/check_pts_idx.py "$IDX" --fps "$fps" --max-zero-pct 0
}

l2_check_p2() {
    python3 tests/check_playout_log.py "$LOG"
}

l2_check_p4() {
    local fps="${1:-23.976}"
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
    FPS="${MP4_DIST_VERIFY_FPS:-23.976}"
    l1_p1
    if [ "${MP4_DIST_SKIP_L2:-0}" != "1" ]; then
        l2_run "$SEC" "$MP4" "$FPS"
        l2_check_pts "$FPS"
    else
        echo "L2: SKIP (MP4_DIST_SKIP_L2=1)"
    fi
    pass "P1"
    ;;
P2|P3)
    SEC="${1:-$SEC}"
    MP4="${2:-$MP4}"
    FPS="${MP4_DIST_VERIFY_FPS:-23.976}"
    l1_p1
    if [ "${MP4_DIST_SKIP_L2:-0}" != "1" ]; then
        l2_run "$SEC" "$MP4" "$FPS"
        l2_check_pts "$FPS"
        l2_check_p2
    else
        echo "L2: SKIP (MP4_DIST_SKIP_L2=1)"
    fi
    pass "$PHASE"
    ;;
P4)
    SEC="${1:-$SEC}"
    MP4="${2:-$MP4}"
    FPS="${MP4_DIST_VERIFY_FPS:-23.976}"
    l1_p1
    if [ -x ./build/test_drm_mode ]; then
        ./build/test_drm_mode || fail "test_drm_mode"
    else
        fail "test_drm_mode missing — ninja -C build"
    fi
    if [ "${MP4_DIST_SKIP_L2:-0}" != "1" ]; then
        l2_run "$SEC" "$MP4" "$FPS"
        l2_check_p4 "$FPS"
    else
        echo "L2: SKIP (MP4_DIST_SKIP_L2=1)"
    fi
    pass "P4"
    ;;
*)
    fail "unknown phase $PHASE (supported: P0, P1, P2, P3, P4)"
    ;;
esac
