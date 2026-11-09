#!/bin/bash
# Usage: netem_inject.sh <iface> <loss%> <delay_ms> <jitter_ms> <reorder%>
# Example: netem_inject.sh lo 1 50 10 0
# Clear:  netem_inject.sh <iface> clear
set -eu
IFACE="${1:?iface}"
if [ "$2" = "clear" ]; then
  tc qdisc del dev "$IFACE" root 2>/dev/null || true
  echo "cleared netem on $IFACE"
  exit 0
fi
LOSS="${2:-0}"
DELAY="${3:-0}"
JITTER="${4:-0}"
REORDER="${5:-0}"
tc qdisc replace dev "$IFACE" root netem loss "${LOSS}%" delay "${DELAY}ms" "${JITTER}ms" reorder "${REORDER}%" 2>&1
echo "applied netem on $IFACE: loss=${LOSS}% delay=${DELAY}ms±${JITTER}ms reorder=${REORDER}%"
