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


def load_thresholds_4k():
    try:
        with open(os.path.join(_HERE, "common", "thresholds.json")) as f:
            return json.load(f).get("rx_decode_4k", {})
    except Exception:
        return {}


_MPP_DEC_RE = re.compile(
    r"au_fed=(\d+) framed=(\d+) no_output=(\d+) put_fail=(\d+)\s+"
    r"mean_us=([\d.]+) p50=(\d+) p90=(\d+) p99=(\d+)"
)


def run_offline_mpp(dec_binary, h264, max_au, duration_hint):
    """用 mpp_dec_latency 探针直测送入到取出耗时（含分位）。"""
    cmd = [dec_binary, "--input", h264]
    if max_au:
        cmd += ["--max-au", str(max_au)]
    print("run: " + " ".join(cmd))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=duration_hint + 120)
    except Exception as ex:
        return {"error": str(ex)}
    text = r.stdout + r.stderr
    m = _MPP_DEC_RE.search(text)
    if not m:
        return {"error": "mpp_dec_latency 输出未解析", "tail": text[-500:]}
    au, framed, no_out, put_fail = (int(m.group(1)), int(m.group(2)),
                                    int(m.group(3)), int(m.group(4)))
    return {
        "au_fed": au, "frames": framed, "no_output": no_out,
        "put_fail": put_fail, "mean_us": float(m.group(5)),
        "p50": int(m.group(6)), "p90": int(m.group(7)),
        "p99": int(m.group(8)), "probe": "mpp_dec_latency",
        "raw_tail": text[-300:],
    }


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
    ap.add_argument("--dec-binary", default=None,
                      help="mpp_dec_latency 探针路径；指定则优先用探针取分位")
    ap.add_argument("--max-au", type=int, default=0,
                      help="offline 探针模式的最大 AU 数（0=探针默认）")
    args = ap.parse_args()
    th = load_thresholds()

    result = {"tool": "rx_decode_perf", "mode": args.mode}
    if args.mode == "offline":
        if not args.input:
            print("[FAIL] offline 模式需要 --input H264")
            return 1
        # 4K 输入（文件名或宽高任一命中）走 4K 阈值
        is_4k = (args.width > 1920 or args.height > 1080
                 or "3840" in args.input or "4k" in args.input.lower())
        if is_4k:
            th = {**th, **load_thresholds_4k()}
            p99_key = ("dec_p99_max_us_30fps" if args.fps <= 32
                         else "dec_p99_max_us_60fps")
            th["dec_p99_max_us"] = th.get(p99_key, th.get("dec_p99_max_us", 66666))
            result["resolution"] = "4k"
        if args.dec_binary and os.path.exists(args.dec_binary):
            result.update(run_offline_mpp(args.dec_binary, args.input,
                                          args.max_au, args.duration))
            if "error" in result:
                print("[FAIL] " + result["error"])
                return 1
            verdict, notes = "PASS", []
            y = result["frames"] / result["au_fed"] if result["au_fed"] else 0
            result["yield"] = round(y, 4)
            # 探针按 start code 切 AU：SPS/PPS/SEI 等非帧 AU 永无输出，
            # 加上解码器 priming，前 ~10 个 no_output 属正常现象，不按 MTL 管线
            # 产出比判定。仅当反压 (put_fail) 或大量丢帧时才 FAIL。
            no_out = result.get("no_output", 0)
            if result["put_fail"] > th.get("put_fail_allowed", 0):
                verdict = "FAIL"
                notes.append("解码反压 put_fail={}".format(result["put_fail"]))
            elif no_out > 12:
                verdict = "FAIL"
                notes.append("无输出 AU {} 过多（priming  allowance 12）".format(no_out))
            if result["p99"] > th.get("dec_p99_max_us", 66666):
                verdict = "FAIL" if verdict == "FAIL" else "WARN"
                notes.append("解码 p99 {}us 超过阈值".format(result["p99"]))
            if not notes:
                notes = ["离线解码 mean={}us p99={}us yield={:.1%}".format(
                    result["mean_us"], result["p99"], y)]
            result["verdict"], result["notes"] = verdict, notes
        else:
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
            if result.get("probe") == "mpp_dec_latency":
                rows = [[result.get("au_fed"), result.get("frames"),
                           result.get("mean_us"), result.get("p50"),
                           result.get("p90"), result.get("p99"),
                           result.get("yield")]]
                hdr = ["au", "framed", "mean(us)", "p50", "p90", "p99", "yield"]
            else:
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
