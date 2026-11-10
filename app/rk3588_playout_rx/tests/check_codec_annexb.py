#!/usr/bin/env python3
"""Classify a raw Annex B dump as h264 or hevc and sanity-check its NALs.

Usage:
  python3 tests/check_codec_annexb.py /tmp/rx_capture.h264 [--expect h264]
  python3 tests/check_codec_annexb.py /tmp/rx_capture.h265 --expect h265

Exit 0 = PASS (and --expect matches when given), 1 = FAIL.

Heuristics, no decoding: split on 3/4-byte start codes, read the first NAL
byte. H264 NAL type is the low 5 bits (SPS=7, PPS=8, SEI=6, IDR=5);
HEVC NAL type is bits 6..1 (VPS=32, SPS=33, PPS=34, SEI prefix=39/suffix=40,
IRAP=16..23). Presence of any HEVC-only type (>31) classifies as hevc;
otherwise H264 parameter sets classify as h264.
"""

import argparse
import sys


def split_nals(buf: bytes):
    """Yield (offset, header_byte) for each Annex B NAL unit."""
    nals = []
    i, n = 0, len(buf)
    while i + 3 < n:
        if buf[i] == 0 and buf[i + 1] == 0 and (
            buf[i + 2] == 1 or (buf[i + 2] == 0 and i + 3 < n and buf[i + 3] == 1)
        ):
            start = i + 3 if buf[i + 2] == 1 else i + 4
            if start < n:
                nals.append((i, buf[start]))
            i = start
        else:
            i += 1
    return nals


def main() -> int:
    ap = argparse.ArgumentParser(description="Check Annex B dump codec")
    ap.add_argument("dump", help="raw Annex B capture file")
    ap.add_argument("--expect", choices=("h264", "h265"),
                    help="required classification")
    ap.add_argument("--min-idr", type=int, default=1,
                    help="minimum IDR/IRAP NALs required")
    args = ap.parse_args()

    try:
        with open(args.dump, "rb") as f:
            buf = f.read()
    except FileNotFoundError:
        print(f"FAIL: dump not found: {args.dump}")
        return 1
    if not buf:
        print(f"FAIL: empty dump {args.dump}")
        return 1

    nals = split_nals(buf)
    if not nals:
        print(f"FAIL: no Annex B start codes in {args.dump}")
        return 1

    h264_sps = h264_pps = h264_idr = h264_sei = 0
    hevc_vps = hevc_sps = hevc_pps = hevc_irap = hevc_sei = 0
    for _, b in nals:
        h264_type = b & 0x1F
        hevc_type = (b >> 1) & 0x3F
        if h264_type == 7:
            h264_sps += 1
        elif h264_type == 8:
            h264_pps += 1
        elif h264_type == 5:
            h264_idr += 1
        elif h264_type == 6:
            h264_sei += 1
        if hevc_type == 32:
            hevc_vps += 1
        elif hevc_type == 33:
            hevc_sps += 1
        elif hevc_type == 34:
            hevc_pps += 1
        elif 16 <= hevc_type <= 23:
            hevc_irap += 1
        elif hevc_type in (39, 40):
            hevc_sei += 1

    hevc_markers = hevc_vps + hevc_sps + hevc_pps
    # Any HEVC-only NAL type wins: H264 types never exceed 31 in the
    # top 6 bits of a shared first byte.
    if hevc_markers > 0:
        codec = "h265"
        key_nals = hevc_irap
        detail = (f"VPS={hevc_vps} SPS={hevc_sps} PPS={hevc_pps} "
                  f"IRAP={hevc_irap} SEI={hevc_sei}")
    else:
        codec = "h264"
        key_nals = h264_idr
        detail = (f"SPS={h264_sps} PPS={h264_pps} "
                  f"IDR={h264_idr} SEI={h264_sei}")

    print(f"dump: {args.dump} ({len(buf)} bytes, {len(nals)} NALs)")
    print(f"  classified: {codec} ({detail})")

    failed = False
    if args.expect and codec != args.expect:
        print(f"FAIL: classified {codec} != expected {args.expect}")
        failed = True
    if key_nals < args.min_idr:
        print(f"FAIL: key NALs {key_nals} < --min-idr {args.min_idr}")
        failed = True
    if codec == "h264" and h264_sps == 0:
        print("FAIL: h264 dump without SPS")
        failed = True
    if codec == "h265" and hevc_markers == 0:
        print("FAIL: h265 dump without parameter sets")
        failed = True

    if failed:
        return 1
    print(f"PASS: annexb codec check ({codec})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
