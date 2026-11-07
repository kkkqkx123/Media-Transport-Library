#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MPP/RGA 硬件负载采样：周期读取驱动计数，外加 CPU 采样。"""
import argparse
import json
import os
import re
import time

_RKVENC_RE = re.compile(r"(rkvenc\S*|rkvdec\S*).*?load:\s*([\d.]+)%\s*utilization:\s*([\d.]+)%")
_RKRGA_RE = re.compile(r"load\s*=\s*([\d.]+)%")


def read_mpp_load():
    """读取各编解码核负载；返回 {core: {load_pct, util_pct}} 或 None。"""
    try:
        with open("/proc/mpp_service/load") as f:
            text = f.read()
    except Exception:
        return None
    out = {}
    for line in text.splitlines():
        m = _RKVENC_RE.search(line)
        if m:
            out[m.group(1)] = {"load_pct": float(m.group(2)), "util_pct": float(m.group(3))}
    return out or None


def read_rkrga_load():
    """读取 RGA 负载；返回 {load_pct} 或 None。"""
    try:
        with open("/sys/kernel/debug/rkrga/load") as f:
            text = f.read()
    except Exception:
        return None
    for line in text.splitlines():
        m = _RKRGA_RE.search(line)
        if m:
            return {"load_pct": float(m.group(1))}
    return None


def read_cpu_pct(interval=0.5):
    """两次 /proc/stat 差值估算整机 CPU 使用率。"""
    def _read():
        with open("/proc/stat") as f:
            parts = f.readline().split()
        vals = [int(v) for v in parts[1:]]
        return sum(vals), vals[3] + (vals[4] if len(vals) > 4 else 0)
    try:
        t0, i0 = _read()
        time.sleep(interval)
        t1, i1 = _read()
        dt = t1 - t0
        return round((1 - (i1 - i0) / dt) * 100, 1) if dt > 0 else None
    except Exception:
        return None


def sample_load(seconds, interval=0.5):
    rows = []
    try:
        with open("/proc/mpp_service/load_interval", "w") as f:
            f.write(str(int(interval * 1000)))
    except Exception:
        pass
    deadline = time.time() + seconds
    while time.time() < deadline:
        rows.append({
            "t": round(time.time() - deadline + seconds, 2),
            "mpp": read_mpp_load(),
            "rkrga": read_rkrga_load(),
        })
        time.sleep(interval)
    return rows


def summarize(rows):
    out = {}
    for key in ("mpp", "rkrga"):
        vals = []
        for r in rows:
            v = r.get(key)
            if not v:
                continue
            if key == "mpp":
                for core in v.values():
                    vals.append(core["load_pct"])
            else:
                vals.append(v["load_pct"])
        out[key] = {
            "samples": len(vals),
            "avg_pct": round(sum(vals) / len(vals), 2) if vals else None,
            "max_pct": round(max(vals), 2) if vals else None,
        }
    return out


def main():
    ap = argparse.ArgumentParser(description="MPP/RGA 硬件负载采样 (板端执行)")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    rows = sample_load(args.seconds, args.interval)
    summary = summarize(rows)
    print("summary: " + json.dumps(summary, ensure_ascii=False))
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w") as f:
            json.dump({"rows": rows, "summary": summary}, f, ensure_ascii=False, indent=2)
        print("written: " + args.out)


if __name__ == "__main__":
    main()
