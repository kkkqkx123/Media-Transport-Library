#!/bin/bash
# Compile verification for split TX/RX binaries.
#
# Usage:
#   ./tests/build_verify.sh          # full build (TX + RX) on RK3588
#   ./tests/build_verify.sh tx-only  # TX-only (x86 / no MPP)
#
set -euo pipefail

CD="$(cd "$(dirname "$0")/.." && pwd)"
cd "$CD"

./tests/setup_meson_options.sh

MODE="${1:-full}"

case "$MODE" in
full)
    echo "=== meson: build_tx + build_rx ==="
    rm -rf build
    meson setup build -Dbuild_tx=true -Dbuild_rx=true
    ninja -C build
    for bin in mp4_dist_tx mp4_dist_rx test_media_clk test_drm_mode; do
        [ -x "./build/$bin" ] || { echo "FAIL: missing build/$bin"; exit 1; }
        echo "  OK: build/$bin"
    done
    ;;
tx-only)
    echo "=== meson: build_tx only (no MPP/DRM/ALSA) ==="
    rm -rf build-tx
    meson setup build-tx -Dbuild_tx=true -Dbuild_rx=false
    ninja -C build-tx
    [ -x ./build-tx/mp4_dist_tx ] || { echo "FAIL: missing build-tx/mp4_dist_tx"; exit 1; }
    echo "  OK: build-tx/mp4_dist_tx"
    [ ! -f ./build-tx/mp4_dist_rx ] && echo "  OK: mp4_dist_rx not built (expected)"
    ;;
*)
    echo "usage: $0 [full|tx-only]"
    exit 1
    ;;
esac

echo "=== build_verify PASS ($MODE) ==="
