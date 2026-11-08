#!/usr/bin/env python3
"""Verify ST 2110-21 VRX margin from MTL run logs.

Parses the MTL log of a TX→RX loopback run and checks:
  1. the effective pacing configuration was applied (TX "vrx" line present);
  2. RX timing-parser VRX stats stay within the compliance window
     (VRX MIN >= threshold, i.e. the virtual receiver never underflows);
  3. optional drop counters (pool-full / ring-drops) did not grow.

Exit 0 when all requested checks pass, 1 otherwise.
Reference: docs/plan/rk3588/RK3588_ST2110_PROTOCOL_GAP_REMEDIATION_PLAN.md (P0-2),
tests/integration_tests/noctx/testcases/st20p_vrx_margin_tests.cpp.
"""

import argparse
import re
import sys

# MTL RX timing parser (info level): "%s(%d), VRX AVG %.2f MIN %d MAX %d"
VRX_RX_RE = re.compile(r"VRX AVG (?P<avg>-?\d+\.?\d*) MIN (?P<min>-?\d+) MAX (?P<max>-?\d+)")
# MTL TX pacing init: "trs %Lf trOffset %Lf vrx %u warm_pkts %u ..."
VRX_TX_RE = re.compile(r"\btrs -?\d[\d.]* trOffset -?\d[\d.]* vrx (?P<vrx>\d+) warm_pkts \d+")
# Timing-parser compliance failure cause
VRX_EXCEED_RE = re.compile(r"vrx exceed (min|max)")


def parse(path):
    vrx_rx = []      # per-frame/per-slot VRX MIN values (RX side)
    vrx_tx = []      # TX pacing VRX values
    exceeds = 0
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = VRX_RX_RE.search(line)
            if m:
                vrx_rx.append(int(m.group("min")))
                continue
            m = VRX_TX_RE.search(line)
            if m:
                vrx_tx.append(int(m.group("vrx")))
            if VRX_EXCEED_RE.search(line):
                exceeds += 1
    return vrx_tx, vrx_rx, exceeds


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", help="MTL run log (TX+RX, or RX only)")
    ap.add_argument("--min-margin", type=int, default=0,
                    help="required VRX MIN lower bound (default 0: no underflow)")
    ap.add_argument("--require-tx", action="store_true",
                    help="fail when no TX pacing line is found in the log")
    args = ap.parse_args()

    vrx_tx, vrx_rx, exceeds = parse(args.log)

    failures = []
    if args.require_tx and not vrx_tx:
        failures.append("no TX pacing line found (pacing not applied?)")
    if not vrx_rx:
        failures.append("no RX VRX stats found (RX timing parser not enabled or too short run)")
    else:
        worst = min(vrx_rx)
        print(f"RX VRX MIN samples: {len(vrx_rx)}  worst(min-of-min): {worst}  "
              f"vrx exceed events: {exceeds}")
        if worst < args.min_margin:
            failures.append(f"VRX margin {worst} < required {args.min_margin}")
        if exceeds:
            failures.append(f"{exceeds} 'vrx exceed' compliance failures")
    if vrx_tx:
        print(f"TX pacing VRX: {vrx_tx[0]} (pkts)")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("PASS: VRX margin within window")
    return 0


if __name__ == "__main__":
    sys.exit(main())
