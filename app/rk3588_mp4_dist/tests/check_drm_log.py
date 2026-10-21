#!/usr/bin/env python3
"""Verify DRM mode in mp4_dist log matches content fps (P4).

Usage:
  python3 tests/check_drm_log.py /tmp/verify_phase.log [--fps 23.976]

Exit 0 = PASS.
"""

import argparse
import re
import sys

sys.path.insert(0, "tests")
# Inline refresh map (mirror shared/drm_mode.c)
def suggest_refresh(fps: float) -> int:
    table = [
        (23.976, 24), (24.0, 24), (25.0, 25), (29.97, 30), (30.0, 30),
        (50.0, 50), (59.94, 60), (60.0, 60), (119.88, 120), (120.0, 120),
    ]
    for f, hz in table:
        if abs(fps - f) < 0.06:
            return hz
    return int(round(fps))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", help="mp4_dist run log")
    ap.add_argument("--fps", type=float, default=0.0)
    args = ap.parse_args()

    text = open(args.log, encoding="utf-8", errors="replace").read()

    fps = args.fps
    if fps <= 0:
        m = re.search(r"@ ([\d.]+)fps", text)
        if m:
            fps = float(m.group(1))
    if fps <= 0:
        fps = 23.976

    expect_hz = suggest_refresh(fps)

    auto = re.search(r"drm_mode: auto → (\d+)x(\d+)@(\d+)", text)
    ready = re.search(r"drm: ready CRTC=\d+ (\d+)x(\d+)@(\d+)", text)

    print(f"log: {args.log}")
    print(f"  content_fps={fps} expect_refresh={expect_hz}Hz")

    failed = False
    if auto:
        aw, ah, av = int(auto.group(1)), int(auto.group(2)), int(auto.group(3))
        print(f"  drm_mode auto → {aw}x{ah}@{av}")
        if av != expect_hz:
            print(f"FAIL: auto refresh {av} != expected {expect_hz}")
            failed = True
    else:
        print("  drm_mode auto line: not found (manual mode?)")

    if ready:
        rw, rh, rv = int(ready.group(1)), int(ready.group(2)), int(ready.group(3))
        print(f"  drm ready → {rw}x{rh}@{rv}")
        if abs(rv - expect_hz) > 1:
            print(f"WARN: actual refresh {rv}Hz differs from expected {expect_hz}Hz "
                  "(driver may not expose exact mode)")
    else:
        print("FAIL: drm: ready line not found")
        failed = True

    if failed:
        return 1
    print("PASS: DRM mode checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
