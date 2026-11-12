#!/bin/bash
# One-way PTP sync verification L1-L3 (single board).
# Location: app/rk3588_playout_rx/tests/; copy to ~/code/mtl/bin on the board.
# Only depends on board binaries + standard tools, not on repo paths.
#
# Usage: run_ptp_sync_verify.sh
# Env: MASTER DOMAIN IFNAME SEC BIN_DIR APP APP_ARGS WITH_TC
#   MASTER  group or unicast master IP (default 224.0.1.129)
#   DOMAIN  PTP domain (default 0; ST 2059-2 uses 127)
#   IFNAME  join interface (default: route choice)
#   SEC     probe + app sample seconds (default 60)
#   APP     app binary for L2 (default file_playout_rx; empty skips L2/L3)
#   APP_ARGS extra app args (must include source/fps; default empty=skip L2/L3)
#   WITH_TC 1 = TC group (offset band 1ms), 0 = plain (band 5ms)
set -euo pipefail

MASTER="${MASTER:-224.0.1.129}"
DOMAIN="${DOMAIN:-0}"
IFNAME="${IFNAME:-}"
SEC="${SEC:-60}"
NOMINAL="${NOMINAL:-20000}"
BIN_DIR="${BIN_DIR:-$HOME/code/mtl/bin}"
APP="${APP:-$BIN_DIR/file_playout_rx}"
APP_ARGS="${APP_ARGS:-}"
WITH_TC="${WITH_TC:-1}"
PROBE_SRC="${PROBE_SRC:-$HOME/code/mtl/src/debug/tests/ptp_sync_probe.c}"
PROBE_BIN="${PROBE_BIN:-$BIN_DIR/ptp_sync_probe}"
LOG_DIR="${LOG_DIR:-/tmp}"
PROBE_LOG="$LOG_DIR/ptp_probe.log"
APP_LOG="$LOG_DIR/ptp_app.log"

fail() { echo "FAIL: $*"; exit 1; }
note() { echo "  $*"; }

command -v gcc >/dev/null || fail "no gcc (board toolchain for probe)"
command -v python3 >/dev/null || fail "no python3 (log checker)"

if [ ! -x "$PROBE_BIN" ]; then
  [ -f "$PROBE_SRC" ] || fail "missing probe src $PROBE_SRC"
  note "building probe"
  gcc -O2 -Wall -o "$PROBE_BIN" "$PROBE_SRC" || fail "probe build"
fi

note "L1: probe $MASTER domain=$DOMAIN ${SEC}s"
"$PROBE_BIN" "$MASTER" "$DOMAIN" "$SEC" | tee "$PROBE_LOG" \
  || fail "L1 no Sync pair (check master/VLAN/IGMP/domain)"
grep -q "^PASS" "$PROBE_LOG" || fail "L1 probe did not PASS"

if [ -z "$APP_ARGS" ]; then
  note "L2/L3 skipped (APP_ARGS empty); L1 PASS"
  echo "PASS (L1 only)"
  exit 0
fi
[ -x "$APP" ] || fail "missing app $APP"

note "L2: app run ${SEC}s"
# shellcheck disable=SC2086
timeout "$SEC" "$APP" $APP_ARGS \
  --ptp-master "$MASTER" ${IFNAME:+--ptp-ifname "$IFNAME"} \
  --ptp-domain "$DOMAIN" --ptp-nominal-delay-ns "$NOMINAL" >"$APP_LOG" 2>&1 || true
grep -q "one_way_ptp: listening" "$APP_LOG" \
  || fail "L2 slave never started (see $APP_LOG)"

if [ "$WITH_TC" = "1" ]; then BAND=1000000; else BAND=5000000; fi
note "L3: check band=${BAND}ns"
python3 "$(dirname "$0")/check_ptp_sync.py" "$APP_LOG" \
  --max-offset-ns "$BAND" || fail "L3 checker FAIL (see $APP_LOG)"
echo "PASS (L1-L3)"
