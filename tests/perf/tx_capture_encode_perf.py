#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""发送侧画像：跑 live_ingest 一段时间，解析 tx_pipe[1s] 行并给出预算判定。

两种输入：默认 HDMI 真实源；--mp4 则用文件帧源（无信号回归）。
判定阈值见 common/thresholds.json。
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "common"))
from hw_load_sample import sample_load, summarize  # noqa: E402
from report import write_summary  # noqa: E402

_TX_RE = re.compile(
    r"tx_pipe\[1s\]: cap=(\d+)/s enc=(\d+)/s \d+kbps .*?"
    r"cap_fps=(\d+) cap_int=(\d+)us .*?"
    r"enc_fps=(\d+) enc_cost=(\d+)us lat=(\d+)/(\d+)/(\d+)us "
    r"qp=(-?\d+)/-?\d+ bps=(-?\d+).*?"
    r"rga ok=(\d+) fail=(\d+) pool_full=(\d+) cpu_fb=(\d+).*?ema=(\d+)us"
)


def parse_tx_line(line):
    m = _TX_RE.search(line)
    if not m:
        return None
    v = [float(x) for x in m.groups()]
    return {
        "cap_per_s": v[0], "enc_per_s": v[1], "cap_fps": v[2],
        "cap_int_us": v[3], "enc_fps": v[4], "enc_cost_us": v[5],
        "enc_p50": v[6], "enc_p90": v[7], "enc_p99": v[8],
        "qp": v[9], "bps_rt": v[10], "rga_ok": v[11],
        "rga_fail": v[12], "rga_pool_full": v[13],
        "rga_cpu_fb": v[14], "rga_ema_us": v[15],
    }


def load_thresholds():
    try:
        with open(os.path.join(_HERE, "common", "thresholds.json")) as f:
            return json.load(f).get("tx_capture_encode", {})
    except Exception:
        return {}


def main():
    ap = argparse.ArgumentParser(description="TX 采集+编码画像 (板端执行)")
    ap.add_argument("--binary", default="app/rk3588_live_tx/build/live_ingest")
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--udp", type=int, default=15000)
    ap.add_argument("--dip", default="127.0.0.1")
    ap.add_argument("--port", default=None, help="MTL TX port name, e.g. native_af_xdp:enP2p33s0")
    ap.add_argument("--mp4", default=None, help="指定则用文件帧源代替 HDMI")
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    th = load_thresholds()

    cmd = [args.binary, str(args.duration), "--udp", str(args.udp),
           "--dip", args.dip]
    if args.port:
        cmd += ["--port", args.port]
    if args.mp4:
        cmd += ["--source", "file", "--mp4", args.mp4, "--loop"]
    print("run: " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1)
    loads = []
    rows = []
    # 边采硬件负载边读日志，避免两次跑引入源抖动差异
    import threading
    stop = {"v": False}

    def _bg():
        deadline = time.time() + args.duration + 5
        while time.time() < deadline and not stop["v"]:
            from hw_load_sample import read_mpp_load, read_rkrga_load
            loads.append({"mpp": read_mpp_load(), "rkrga": read_rkrga_load()})
            time.sleep(0.5)

    t = threading.Thread(target=_bg, daemon=True)
    t.start()
    assert proc.stdout is not None
    try:
        for line in proc.stdout:
            r = parse_tx_line(line)
            if r:
                rows.append(r)
                print("enc_fps={enc_fps} cost={enc_cost_us}us "
                      "p99={enc_p99}us cap_int={cap_int_us}us rga_ema={rga_ema_us}us".format(**r))
        proc.wait()
    except KeyboardInterrupt:
        print("\n[INT] 中断，停 live_ingest 后退出…")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        raise SystemExit(130)
    finally:
        stop["v"] = True
        t.join(timeout=3)
        if proc.poll() is None:
            proc.kill()

    if not rows:
        print("[FAIL] 未解析到 tx_pipe[1s] 行（检查 HDMI 信号或二进制路径）")
        return 1

    def mean(k):
        return round(sum(r[k] for r in rows) / len(rows), 1)

    def mx(k):
        return max(r[k] for r in rows)

    load_sum = summarize(loads) if loads else {}
    enc_ratio = mean("enc_fps") / args.fps if args.fps > 0 else 0
    cap_fps = mean("cap_fps")
    verdict, notes = "PASS", []
    if enc_ratio < th.get("encode_fps_ratio", 0.95):
        verdict = "FAIL"
        notes.append("编码未跟上目标帧率 {:.0%}".format(enc_ratio))
    if mx("enc_p99") > th.get("enc_p99_max_us", 33333):
        verdict = "FAIL" if verdict == "FAIL" else "WARN"
        notes.append("编码 p99 超过两帧间隔")
    rkvenc_max = ((load_sum.get("mpp") or {}).get("max_pct"))
    if rkvenc_max is not None and rkvenc_max >= th.get("rkvenc_saturation_pct", 90.0):
        notes.append("rkvenc 触顶 {:.1f}%（上限形态，非预算形态）".format(rkvenc_max))

    result = {
        "tool": "tx_capture_encode_perf",
        "source": "file:{}".format(args.mp4) if args.mp4 else "hdmi",
        "samples": len(rows),
        "cap_fps_mean": cap_fps,
        "cap_int_us_mean": mean("cap_int_us"),
        "enc_fps_mean": mean("enc_fps"),
        "enc_ratio": round(enc_ratio, 3),
        "enc_cost_us_mean": mean("enc_cost_us"),
        "enc_p50_max": mx("enc_p50"),
        "enc_p90_max": mx("enc_p90"),
        "enc_p99_max": mx("enc_p99"),
        "rga_ema_us_mean": mean("rga_ema_us"),
        "rga_pool_full_total": int(sum(r["rga_pool_full"] for r in rows)),
        "qp_mean": mean("qp"),
        "bps_rt_mean": mean("bps_rt"),
        "load": load_sum,
        "verdict": verdict,
        "notes": notes,
    }
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "tx_capture_encode.json"), "w") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        write_summary(
            os.path.join(args.out, "tx_capture_encode.md"),
            "TX 采集+编码画像",
            meta={"输入": result["source"], "样本": len(rows),
                  "结论": verdict + ("：" + "；".join(notes) if notes else "")},
            table_header=["cap_fps", "cap_int(us)", "enc_fps", "enc_cost(us)",
                          "p50", "p90", "p99", "rga_ema(us)", "rkvenc_max(%)"],
            table_rows=[[cap_fps, mean("cap_int_us"), mean("enc_fps"),
                         mean("enc_cost_us"), mx("enc_p50"), mx("enc_p90"),
                         mx("enc_p99"), mean("rga_ema_us"),
                         rkvenc_max if rkvenc_max is not None else "-"]])
        print("written: " + args.out)
    return 0 if verdict != "FAIL" else 1


if __name__ == "__main__":
    raise SystemExit(main())
