#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""接收侧画像：离线预置流解码，或接真实 MTL 组播收流。

offline 模式隔离了解码器本身的吞吐；mtl 模式验证收流链条等速。
判定阈值见 common/thresholds.json。
"""
import argparse
import json
import os
import re
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "common"))
from report import write_summary  # noqa: E402

_RX_RE = re.compile(
    r"RX\[[^\]]+\]: mtl_rx=\+(\d+) bridge=\+(\d+) dec=\+(\d+) render=\+(\d+) "
    r"mtl_drop=\+(\d+) br_drop=\+(\d+) corrupt=(\d+) incomplete=(\d+)"
)
_RX_TOTAL_RE = re.compile(
    r"RX\[[^\]]+\] total: pkt=(\d+) dec=(\d+) render=(\d+) "
    r"pb_skip=(\d+) pb_underrun=(\d+)"
)
_E2E_RE = re.compile(
    r"E2E-C[^:]*: n=(\d+) min=([\d.]+)ms avg=([\d.]+)ms max=([\d.]+)ms.*?"
    r"skew_reject=(\d+)"
)


def load_thresholds():
    try:
        with open(os.path.join(_HERE, "common", "thresholds.json")) as f:
            return json.load(f).get("rx_decode", {})
    except Exception:
        return {}


def parse_rx_log(text):
    periods, totals, e2es = [], [], []
    for line in text.splitlines():
        m = _RX_RE.search(line)
        if m:
            periods.append([int(x) for x in m.groups()])
        m = _RX_TOTAL_RE.search(line)
        if m:
            totals.append([int(x) for x in m.groups()])
        m = _E2E_RE.search(line)
        if m:
            e2es.append({
                "n": int(m.group(1)), "min_ms": float(m.group(2)),
                "avg_ms": float(m.group(3)), "max_ms": float(m.group(4)),
                "skew": int(m.group(5)),
            })
    return periods, totals, e2es


def run_offline(h264, duration_hint):
    """用 mpi_dec_test 回放预置流，取解码 fps。"""
    cmd = ["mpi_dec_test", "-t", "7", "-i", h264, "-n", "0"]
    print("run: " + " ".join(cmd))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=duration_hint + 60)
    except Exception as ex:
        return {"error": str(ex)}
    text = r.stdout + r.stderr
    m = re.search(r"decode\s+(\d+)\s+frames.*?fps\s+([\d.]+)", text)
    if not m:
        return {"error": "mpi_dec_test 输出未解析", "tail": text[-500:]}
    return {"frames": int(m.group(1)), "fps": float(m.group(2)), "raw_tail": text[-500:]}


def run_mtl(binary, duration, udp, sip, width, height, fps):
    cmd = [binary, str(duration), "--udp", str(udp), "--sip", sip,
           "--width", str(width), "--height", str(height),
           "--fps", str(fps)]
    print("run: " + " ".join(cmd))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=duration + 60)
    except Exception as ex:
        return {"error": str(ex)}
    return {"log": r.stdout + r.stderr, "rc": r.returncode}


def main():
    ap = argparse.ArgumentParser(description="RX 收流+解码画像 (板端执行)")
    ap.add_argument("--mode", choices=["offline", "mtl"], default="offline")
    ap.add_argument("--input", default=None, help="offline 模式的 H264 文件")
    ap.add_argument("--binary", default="app/rk3588_playout_rx/build/file_playout_rx")
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--udp", type=int, default=15000)
    ap.add_argument("--sip", default="127.0.0.1")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    th = load_thresholds()

    result = {"tool": "rx_decode_perf", "mode": args.mode}
    if args.mode == "offline":
        if not args.input:
            print("[FAIL] offline 模式需要 --input H264")
            return 1
        result.update(run_offline(args.input, args.duration))
        if "error" in result:
            print("[FAIL] " + result["error"])
            return 1
        result["verdict"] = "PASS"
        result["notes"] = ["离线解码 fps={}".format(result["fps"])]
    else:
        r = run_mtl(args.binary, args.duration, args.udp, args.sip,
                    args.width, args.height, args.fps)
        if "error" in r:
            print("[FAIL] " + r["error"])
            return 1
        periods, totals, e2es = parse_rx_log(r["log"])
        if not totals:
            print("[FAIL] 未解析到 RX total 行（检查发送端与组播）")
            print(r["log"][-2000:])
            return 1
        pkt, dec, render, skip, under = totals[-1]
        y = dec / pkt if pkt else 0
        result.update({
            "pkt": pkt, "dec": dec, "render": render,
            "pb_skip": skip, "pb_underrun": under,
            "yield": round(y, 4),
            "e2e": e2es[-1] if e2es else None,
        })
        verdict, notes = "PASS", []
        if y < th.get("yield_ratio", 0.99):
            verdict, notes = "FAIL", ["解码产出比 {:.1%} 偏低".format(y)]
        if under > th.get("pb_underrun_allowed", 0):
            verdict = "FAIL" if verdict == "FAIL" else "WARN"
            notes.append("抖动缓冲欠载 {}（playout_delay 偏小）".format(under))
        if e2es and e2es[-1]["skew"] > th.get("skew_reject_allowed", 0):
            verdict, notes = "FAIL", notes + ["时钟偏斜，本轮 E2E 作废"]
        result["verdict"], result["notes"] = verdict, notes

    print(json.dumps(result, ensure_ascii=False, indent=2))
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "rx_decode.json"), "w") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        if args.mode == "offline":
            rows = [[result.get("frames"), result.get("fps")]]
            hdr = ["frames", "fps"]
        else:
            rows = [[result.get("pkt"), result.get("dec"), result.get("render"),
                     result.get("yield"), result.get("pb_underrun"),
                     (result.get("e2e") or {}).get("avg_ms", "-")]]
            hdr = ["pkt", "dec", "render", "yield", "underrun", "e2e_avg(ms)"]
        write_summary(
            os.path.join(args.out, "rx_decode.md"), "RX 收流+解码画像",
            meta={"模式": args.mode,
                  "结论": result["verdict"] + ("：" + "；".join(result["notes"]) if result.get("notes") else "")},
            table_header=hdr, table_rows=rows)
        print("written: " + args.out)
    return 0 if result.get("verdict") != "FAIL" else 1


if __name__ == "__main__":
    raise SystemExit(main())
