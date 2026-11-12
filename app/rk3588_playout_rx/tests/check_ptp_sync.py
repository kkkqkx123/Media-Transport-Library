#!/usr/bin/env python3
"""Check one-way PTP slave convergence from app logs.

Usage:
  python3 tests/check_ptp_sync.py RX_LOG [--tx-log TX_LOG]
      [--max-offset-ns N] [--max-rej-ratio R] [--expect-holdover]

Exit 0 = PASS, 1 = FAIL.

Parses `one_way_ptp:` status lines (10s cadence) and `PTP[]` startup lines:
  - slave ran (listening line present)
  - servo locked at least once
  - last offset within band (default 1ms with TC, pass 5ms without)
  - reject ratio bounded (software RX stamp jitter under load)
  - last state not holdover (unless --expect-holdover for L5 runs)
With --tx-log, both logs must independently pass (same master required
only when both sides see Announce traffic; not enforced here).
"""

import argparse
import re
import sys

LISTEN_RE = re.compile(r"one_way_ptp: listening (\S+) domain=(\d+)")
STATUS_RE = re.compile(
    r"one_way_ptp: (locked|free)(\+holdover)? offset=(-?\d+) ns "
    r"drift=(-?\d+) ppb sync=(\d+) rej=(\d+)"
)
LOCKED_RE = re.compile(r"one_way_ptp: locked \(offset (-?\d+) ns\)")
HOLDOVER_RE = re.compile(r"one_way_ptp: no Sync for \d+ ms, holdover")
BACK_RE = re.compile(r"one_way_ptp: master back, leaving holdover")
PTP_RE = re.compile(r"PTP\[(.*?)\]: .* corr=(-?\d+) ns (locked|free)"
                    r"(\+holdover)?")


def check_one(path, args):
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError as e:
        print(f"FAIL: cannot read {path}: {e}")
        return 1

    m = LISTEN_RE.search(text)
    if not m:
        print(f"FAIL: {path}: no one_way_ptp listening line (slave not started?)")
        return 1
    print(f"  {path}: master={m.group(1)} domain={m.group(2)}")

    statuses = STATUS_RE.findall(text)
    if not statuses:
        print(f"FAIL: {path}: no one_way_ptp status lines")
        return 1
    locked_seen = any(s[0] == "locked" for s in statuses) or bool(
        LOCKED_RE.search(text)
    )
    if not locked_seen:
        print(f"FAIL: {path}: servo never locked")
        return 1

    last = statuses[-1]
    state, hold, off, drift, sync, rej = (
        last[0], last[1], int(last[2]), int(last[3]),
        int(last[4]), int(last[5]),
    )
    if abs(off) > args.max_offset_ns:
        print(f"FAIL: {path}: last offset {off} ns exceeds "
              f"{args.max_offset_ns} ns")
        return 1
    total = sync + rej
    ratio = (rej / total) if total else 1.0
    if total == 0 or ratio > args.max_rej_ratio:
        print(f"FAIL: {path}: reject ratio {ratio:.3f} "
              f"(sync={sync} rej={rej}) exceeds {args.max_rej_ratio}")
        return 1
    if hold and not args.expect_holdover:
        print(f"FAIL: {path}: ends in holdover (L5 run? pass "
              f"--expect-holdover)")
        return 1
    holds = len(HOLDOVER_RE.findall(text))
    backs = len(BACK_RE.findall(text))
    print(f"  {path}: PASS state={state}{hold} offset={off}ns "
          f"drift={drift}ppb sync={sync} rej={rej} "
          f"holdovers={holds} recoveries={backs}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", help="app run log (TX or RX)")
    ap.add_argument("--tx-log", default=None,
                    help="second log for dual-board runs (each checked)")
    ap.add_argument("--max-offset-ns", type=int, default=1000000,
                    help="last-offset band (1ms with TC, use 5000000 without)")
    ap.add_argument("--max-rej-ratio", type=float, default=0.10)
    ap.add_argument("--expect-holdover", action="store_true",
                    help="L5 holdover runs may end in holdover")
    args = ap.parse_args()

    rc = check_one(args.log, args)
    if args.tx_log:
        rc |= check_one(args.tx_log, args)
    print("PASS" if rc == 0 else "FAIL")
    return rc


if __name__ == "__main__":
    sys.exit(main())
