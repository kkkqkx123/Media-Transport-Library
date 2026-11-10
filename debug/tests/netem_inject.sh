#!/bin/bash
# Usage: netem_inject.sh <iface> <loss%> <delay_ms> <jitter_ms> <reorder%>
# Example: netem_inject.sh lo 1 50 10 0
# Clear:  netem_inject.sh <iface> clear
set -euo pipefail
IFACE="${1:?用法: $0 <iface> <loss%> <delay_ms> <jitter_ms> <reorder%> | $0 <iface> clear}"
[ $# -ge 2 ] || { echo "用法: $0 <iface> <loss%> <delay_ms> <jitter_ms> <reorder%> | $0 <iface> clear"; exit 1; }
command -v tc >/dev/null 2>&1 || { echo "缺 tc（iproute2）"; exit 1; }
ip link show "$IFACE" >/dev/null 2>&1 || { echo "网卡 $IFACE 不存在"; exit 1; }
if [ "${2:-}" = "clear" ]; then
  tc qdisc del dev "$IFACE" root 2>/dev/null || true
  echo "cleared netem on $IFACE"
  tc qdisc show dev "$IFACE"
  exit 0
fi
LOSS="${2:-0}"
DELAY="${3:-0}"
JITTER="${4:-0}"
REORDER="${5:-0}"
tc qdisc replace dev "$IFACE" root netem loss "${LOSS}%" delay "${DELAY}ms" "${JITTER}ms" reorder "${REORDER}%" 2>&1
tc qdisc show dev "$IFACE"
echo "applied netem on $IFACE: loss=${LOSS}% delay=${DELAY}ms±${JITTER}ms reorder=${REORDER}%"
