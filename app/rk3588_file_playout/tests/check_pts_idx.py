#!/usr/bin/env python3
"""Validate RX capture index PTS (size pts_ns per line).

Usage:
  python3 tests/check_pts_idx.py /tmp/rx_capture.idx [--fps FPS] [--max-zero-pct N]

Exit 0 = PASS, 1 = FAIL.
"""

import argparse
import sys
from typing import List, Tuple


def load_idx(path: str) -> List[Tuple[int, int]]:
    rows: List[Tuple[int, int]] = []
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 2:
                print(f"FAIL: {path}:{lineno}: bad line {line!r}")
                sys.exit(1)
            try:
                size = int(parts[0])
                pts = int(parts[1])
            except ValueError as e:
                print(f"FAIL: {path}:{lineno}: {e}")
                sys.exit(1)
            rows.append((size, pts))
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description="Check rx_capture.idx PTS quality")
    ap.add_argument("idx", help="index file (size pts_ns per line)")
    ap.add_argument("--fps", type=float, default=0.0, help="expected fps for step check")
    ap.add_argument("--max-zero-pct", type=float, default=0.0,
                    help="max allowed zero PTS percent (default 0)")
    ap.add_argument("--step-tolerance-pct", type=float, default=15.0,
                    help="PTS step tolerance vs 1e9/fps")
    args = ap.parse_args()

    try:
        rows = load_idx(args.idx)
    except FileNotFoundError:
        print(f"FAIL: index not found: {args.idx}")
        return 1

    if not rows:
        print(f"FAIL: empty index {args.idx}")
        return 1

    n = len(rows)
    zeros = sum(1 for _, pts in rows if pts == 0)
    zero_pct = 100.0 * zeros / n

    non_mono = 0
    steps: List[int] = []
    prev = rows[0][1]
    for _, pts in rows[1:]:
        if pts < prev:
            non_mono += 1
        if pts > prev:
            steps.append(pts - prev)
        prev = pts

    print(f"idx: {args.idx}")
    print(f"  packets:     {n}")
    print(f"  zero_pts:    {zeros} ({zero_pct:.2f}%)")
    print(f"  non_mono:    {non_mono}")
    if steps:
        avg_step = sum(steps) / len(steps)
        print(f"  avg_step_ns: {avg_step:.0f}")
        if args.fps > 0:
            expect = 1e9 / args.fps
            err_pct = abs(avg_step - expect) / expect * 100.0
            print(f"  expect_step: {expect:.0f} ns @ {args.fps} fps (err {err_pct:.1f}%)")

    failed = False
    if zero_pct > args.max_zero_pct:
        print(f"FAIL: zero_pts {zero_pct:.2f}% > max {args.max_zero_pct}%")
        failed = True
    if non_mono > 0:
        print(f"FAIL: {non_mono} non-monotonic PTS transitions")
        failed = True
    if args.fps > 0 and steps:
        expect = 1e9 / args.fps
        avg_step = sum(steps) / len(steps)
        err_pct = abs(avg_step - expect) / expect * 100.0
        if err_pct > args.step_tolerance_pct:
            print(f"WARN: avg step err {err_pct:.1f}% > {args.step_tolerance_pct}%")

    if failed:
        return 1
    print("PASS: PTS index checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
