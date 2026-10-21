#!/usr/bin/env python3
"""Compare MP4_DIST_AU_TRACE CSVs (single-board both mode).

Usage:
  python3 tests/compare_au_trace.py /tmp/run1

Aligns streams by (size, crc32) subsequence — not by row index.
"""

import csv
import io
import os
import sys
from dataclasses import dataclass
from typing import List, Optional, Tuple


@dataclass
class AuRec:
    seq: int
    stage: str
    size: int
    crc32: int
    nal0: int
    idr: int
    sps: int
    extra: int


def _read_text_strip_nul(path: str) -> str:
    with open(path, "rb") as f:
        raw = f.read()
    if not raw:
        return ""
    nul = raw.count(b"\x00")
    if nul:
        print(f"  WARN: {path}: {nul} NUL byte(s) — re-run after rm /tmp/run1*.csv")
        raw = raw.replace(b"\x00", b"")
    if raw[:4] == b"\x00\x00\x00\x01" or raw[:3] == b"\x00\x00\x01":
        raise ValueError(f"{path} looks like raw H.264, not CSV")
    return raw.decode("utf-8", errors="replace")


def load(path: str) -> List[AuRec]:
    text = _read_text_strip_nul(path)
    if not text.strip():
        return []
    if not text.lstrip().startswith("seq,"):
        preview = text.splitlines()[0][:80] if text.splitlines() else ""
        raise ValueError(f"{path}: missing CSV header (first line: {preview!r})")

    out: List[AuRec] = []
    r = csv.DictReader(io.StringIO(text))
    need = {"seq", "stage", "size", "crc32", "nal0", "idr", "sps", "extra"}
    if not need.issubset(set(r.fieldnames or [])):
        raise ValueError(f"{path}: bad columns {r.fieldnames}")

    for lineno, row in enumerate(r, start=2):
        try:
            out.append(
                AuRec(
                    seq=int(row["seq"]),
                    stage=row["stage"],
                    size=int(row["size"]),
                    crc32=int(row["crc32"], 16),
                    nal0=int(row["nal0"]),
                    idr=int(row["idr"]),
                    sps=int(row["sps"]),
                    extra=int(row["extra"]),
                )
            )
        except (KeyError, ValueError) as e:
            raise ValueError(f"{path}:{lineno}: {e} row={row!r}") from e
    return out


def key(rec: AuRec) -> Tuple[int, int]:
    return rec.size, rec.crc32


def align_subseq(ref: List[AuRec], other: List[AuRec]) -> List[Tuple[int, int]]:
    """Match each item in other to ref in order (subsequence alignment)."""
    pairs: List[Tuple[int, int]] = []
    j = 0
    for oi, orec in enumerate(other):
        ok = key(orec)
        while j < len(ref) and key(ref[j]) != ok:
            j += 1
        if j >= len(ref):
            break
        pairs.append((j, oi))
        j += 1
    return pairs


def find_offset(ref: List[AuRec], other: List[AuRec]) -> Optional[int]:
    if not ref or not other:
        return None
    target = key(other[0])
    for i, r in enumerate(ref):
        if key(r) == target:
            return i
    return None


def report_pairs(name: str, ref: List[AuRec], other: List[AuRec],
                 pairs: List[Tuple[int, int]], limit: int = 20) -> None:
    mism = []
    for ri, oi in pairs:
        if key(ref[ri]) != key(other[oi]):
            mism.append((ri, oi, ref[ri], other[oi]))
    print(f"{name}:")
    print(f"  ref records:        {len(ref)}")
    print(f"  other records:      {len(other)}")
    print(f"  aligned pairs:      {len(pairs)}")
    print(f"  exact mismatches:   {len(mism)}")
    if pairs:
        rate = 100.0 * (len(pairs) - len(mism)) / len(pairs)
        print(f"  match rate:         {rate:.1f}%")
    for ri, oi, a, b in mism[:limit]:
        print(
            f"    ref#{ri+1} vs other#{oi+1}: "
            f"size {a.size}/{b.size} crc {a.crc32:08x}/{b.crc32:08x}"
        )
    if len(mism) > limit:
        print(f"    ... {len(mism) - limit} more")


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} TRACE_BASE", file=sys.stderr)
        return 1

    base = sys.argv[1].rstrip("/")
    paths = {
        "tx": f"{base}.tx.csv",
        "tx_wire": f"{base}.tx_wire.csv",
        "rx_mtl": f"{base}.rx_mtl.csv",
        "rx_br": f"{base}.rx_br.csv",
    }

    data = {}
    for name, p in paths.items():
        try:
            data[name] = load(p)
            sz = os.path.getsize(p) if os.path.isfile(p) else 0
            print(f"{name:8} {len(data[name]):6} records  ({p}, {sz} B)")
        except FileNotFoundError:
            print(f"{name:8} MISSING ({p})")
            data[name] = []
        except ValueError as e:
            print(f"{name:8} ERROR: {e}")
            data[name] = []

    tx = data.get("tx", [])
    tx_wire = data.get("tx_wire", [])
    rx_mtl = data.get("rx_mtl", [])
    rx_br = data.get("rx_br", [])

    print()
    if tx and tx_wire:
        pairs = align_subseq(tx, tx_wire)
        report_pairs("TX publish vs TX wire (subsequence align)", tx, tx_wire, pairs)
        if len(pairs) < min(len(tx), len(tx_wire)) * 0.95:
            print("  hint: count gap may mean duplicate wire trace or dropped publish")

    if rx_mtl and rx_br:
        pairs = align_subseq(rx_mtl, rx_br)
        report_pairs("RX mtl vs RX bridge", rx_mtl, rx_br, pairs)

    print()
    if tx_wire and rx_br:
        off_w = find_offset(tx_wire, rx_br)
        pairs_w = align_subseq(tx_wire, rx_br)
        if off_w is not None:
            n = min(len(rx_br), len(tx_wire) - off_w)
            match_w = sum(
                1 for i in range(n) if key(tx_wire[off_w + i]) == key(rx_br[i])
            )
            print(f"TX wire vs RX bridge (tx_wire starts at #{off_w + 1}):")
            print(f"  tx_wire records:    {len(tx_wire)}")
            print(f"  rx_br records:      {len(rx_br)}")
            print(f"  overlap compare:    {n}")
            print(f"  exact match:        {match_w}/{n} ({100.0 * match_w / max(n, 1):.1f}%)")
            for i in range(min(10, n)):
                if key(tx_wire[off_w + i]) != key(rx_br[i]):
                    t, r = tx_wire[off_w + i], rx_br[i]
                    print(
                        f"    first mismatch rx#{i + 1}: wire size={t.size} crc={t.crc32:08x}"
                        f"  rx size={r.size} crc={r.crc32:08x}"
                    )
                    break
        else:
            print("TX wire vs RX bridge: first RX pkt not found in tx_wire")
            print(f"  subsequence pairs:  {len(pairs_w)}/{len(rx_br)}")
        print(f"  (subsequence pairs: {len(pairs_w)}, mismatches in pairs: "
              f"{sum(1 for ri, oi in pairs_w if key(tx_wire[ri]) != key(rx_br[oi]))})")

    print()
    if tx and rx_br:
        off = find_offset(tx, rx_br)
        if off is None:
            pairs = align_subseq(tx, rx_br)
            print("TX publish vs RX bridge: first RX pkt not in TX (no offset)")
            print(f"  subsequence pairs:  {len(pairs)}/{len(rx_br)}")
            if pairs:
                print(f"  first aligned at tx#{pairs[0][0]+1} rx#{pairs[0][1]+1}")
        else:
            print(f"TX publish vs RX bridge (tx starts at ref#{off+1}):")
            n = min(len(rx_br), len(tx) - off)
            match = sum(1 for i in range(n) if key(tx[off + i]) == key(rx_br[i]))
            print(f"  aligned length:     {n}")
            print(f"  exact match:        {match}/{n} ({100.0 * match / max(n, 1):.1f}%)")
            for i in range(min(10, n)):
                if key(tx[off + i]) != key(rx_br[i]):
                    t, r = tx[off + i], rx_br[i]
                    print(
                        f"    first mismatch rx#{i+1}: tx size={t.size} crc={t.crc32:08x}"
                        f"  rx size={r.size} crc={r.crc32:08x}"
                    )
                    break

    if not tx:
        print()
        print("NOTE: tx.csv missing — use mp4_dist_both.sh, or run TX with MP4_DIST_AU_TRACE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
