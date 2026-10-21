#!/usr/bin/env bash
# Start linuxptp (unicast) for PC↔RK dual-host mp4_dist.
# Requires linuxptp >= 2.0 (ptp4l -v). Use /usr/local/sbin if built from source.
#
# Usage:
#   On PC (master):  ./scripts/start_ptp_dual_host.sh master enp1s0
#   On RK (slave):   ./scripts/start_ptp_dual_host.sh slave eth0 [MASTER_IP]

set -euo pipefail

ROLE="${1:-}"
IFACE="${2:-}"
MASTER_IP="${3:-192.168.80.213}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PTP4L="$(command -v ptp4l || true)"
PHC2SYS="$(command -v phc2sys || true)"

for p in /usr/local/sbin/ptp4l /usr/sbin/ptp4l; do
  [[ -x "$p" ]] && PTP4L="$p" && break
done
for p in /usr/local/sbin/phc2sys /usr/sbin/phc2sys; do
  [[ -x "$p" ]] && PHC2SYS="$p" && break
done

if [[ -z "$ROLE" || -z "$IFACE" ]]; then
  echo "Usage: $0 master|slave IFACE [MASTER_IP]" >&2
  exit 1
fi

VER="$("$PTP4L" -v 2>&1 | head -1 || true)"
echo "ptp4l version: $VER"
case "$VER" in
  1.*)
    echo "ERROR: linuxptp 1.x does not support unicast. Install >= 2.0 from source." >&2
    exit 1
    ;;
esac

sudo systemctl stop chrony systemd-timesyncd 2>/dev/null || true

TAI_TOOL="$ROOT/../../tools/set_tai_offset/build/set_tai_offset"
if [[ -x "$TAI_TOOL" ]]; then
  sudo "$TAI_TOOL" || true
elif command -v adjtimex >/dev/null; then
  sudo adjtimex --tai 37 2>/dev/null || true
fi

case "$ROLE" in
  master)
    CONF="$ROOT/config/ptp/ptp4l-master-unicast.conf"
    echo "Starting PTP master on $IFACE ..."
    exec sudo "$PTP4L" -f "$CONF" -i "$IFACE" -m -S
    ;;
  slave)
    CONF="$ROOT/config/ptp/ptp4l-slave-unicast.conf"
    if [[ "$MASTER_IP" != "192.168.80.213" ]]; then
      TMP="$(mktemp)"
      sed "s/192.168.80.213/$MASTER_IP/g" "$CONF" > "$TMP"
      CONF="$TMP"
    fi
    echo "Starting PTP slave on $IFACE → master $MASTER_IP ..."
    sudo "$PTP4L" -f "$CONF" -i "$IFACE" -s -S -m &
    sleep 3
    exec sudo "$PHC2SYS" -a -r -m
    ;;
  *)
    echo "Unknown role: $ROLE" >&2
    exit 1
    ;;
esac
