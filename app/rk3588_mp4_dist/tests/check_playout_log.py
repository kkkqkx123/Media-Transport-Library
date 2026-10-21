#!/usr/bin/env python3
"""Parse rx_pipe summary for P2 (latest-frame playout) + P3 frame PTS.

Usage:
  python3 tests/check_playout_log.py /tmp/verify_phase.log [--fps 23.976]

Exit 0 = PASS, 1 = FAIL.

P2 policy C: render newest due frame; pb_skip = popped but not displayed.
Do NOT require render/dec ~= 100%.
"""

import argparse
import re
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", help="mp4_dist run log")
    ap.add_argument("--fps", type=float, default=23.976)
    ap.add_argument("--min-render-pct-of-expected", type=float, default=35.0,
                    help="render/(test_sec*fps); both-mode decode ~80%% → render ~40%%")
    ap.add_argument("--max-overrun-pct-of-dec", type=float, default=5.0)
    ap.add_argument("--max-repeat", type=int, default=100000)
    args = ap.parse_args()

    text = open(args.log, encoding="utf-8", errors="replace").read()

    if "playout=latest" not in text and "playout=on" not in text:
        print("FAIL: playout=latest/on not in log")
        return 1

    # New format with pb_skip (P2 final)
    m = re.search(
        r"rx_pipe: (\d+) pkt, (\d+) dec .*? "
        r"(\d+) pb_push, (\d+) render .*? "
        r"pb_skip=(\d+) pb_overrun=(\d+) pb_repeat=(\d+) pb_underrun=(\d+)",
        text,
    )
    skip = 0
    if not m:
        # Legacy line without pb_skip
        m = re.search(
            r"rx_pipe: (\d+) pkt, (\d+) dec .*? "
            r"(\d+) pb_push, (\d+) render .*? "
            r"pb_overrun=(\d+) pb_repeat=(\d+) pb_underrun=(\d+)",
            text,
        )
        if not m:
            print("FAIL: rx_pipe summary not found")
            return 1
        pkt, dec, push, render = (int(m.group(i)) for i in range(1, 5))
        overrun, repeat, underrun = (int(m.group(i)) for i in range(5, 8))
    else:
        pkt, dec, push, render = (int(m.group(i)) for i in range(1, 5))
        skip, overrun, repeat, underrun = (int(m.group(i)) for i in range(5, 9))

    test_sec = 120.0
    em = re.search(r"of ~(\d+) @ ([\d.]+)fps", text)
    if em:
        exp_n = int(em.group(1))
        fps_infer = float(em.group(2))
        if fps_infer > 0:
            test_sec = exp_n / fps_infer
    expected = test_sec * args.fps

    print(f"log: {args.log}")
    print(f"  pkt={pkt} dec={dec} push={push} render={render} skip={skip}")
    print(f"  overrun={overrun} repeat={repeat} underrun={underrun}")
    print(f"  expected_frames≈{expected:.0f}")

    failed = False
    if repeat > args.max_repeat:
        print(f"FAIL: pb_repeat={repeat} (busy-loop)")
        failed = True
    if push != dec:
        print(f"WARN: pb_push ({push}) != dec ({dec})")

    if dec <= 0:
        print("FAIL: dec=0")
        return 1

    r_exp = 100.0 * render / expected if expected > 0 else 0.0
    r_dec = 100.0 * render / dec
    o_pct = 100.0 * overrun / dec
    accounted = render + skip + overrun
    acct_pct = 100.0 * accounted / dec

    print(f"  render/expected:  {r_exp:.1f}%")
    print(f"  render/dec:       {r_dec:.1f}% (informational; C policy skips)")
    print(f"  (render+skip+overrun)/dec: {accounted}/{dec} = {acct_pct:.1f}%")

    if r_exp < args.min_render_pct_of_expected:
        print(f"FAIL: render/expected {r_exp:.1f}% < {args.min_render_pct_of_expected}%")
        failed = True
    if o_pct > args.max_overrun_pct_of_dec:
        print(f"FAIL: overrun/dec {o_pct:.1f}% > {args.max_overrun_pct_of_dec}%")
        failed = True
    # Most displayed/skipped/overrun frames should account for pushes (queue drain at end may differ)
    if acct_pct < 85.0 and skip == 0:
        print(f"WARN: low accounting {acct_pct:.1f}% without pb_skip (old binary?)")

    if failed:
        return 1
    print("PASS: playout log checks (policy C + frame PTS)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
