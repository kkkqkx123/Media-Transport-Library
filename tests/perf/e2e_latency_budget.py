#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""端到端预算：单板回环 TX+RX，拼出采集/编码/解码呈现三段预算表。

默认同机回环（单调时钟可比）；双板对比需 PTP 就绪且 skew 为零。
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
from report import write_summary  # noqa: E402
from tx_capture_encode_perf import parse_tx_line  # noqa: E402
from rx_decode_perf import parse_rx_log  # noqa: E402


def load_thresholds():
    try:
        with open(os.path.join(_HERE, "common", "thresholds.json")) as f:
            return json.load(f).get("e2e", {})
    except Exception:
        return {}


def main():
    ap = argparse.ArgumentParser(description="E2E 延迟预算 (板端执行)")
    ap.add_argument("--tx-binary", default="app/rk3588_live_tx/build/live_ingest")
    ap.add_argument("--rx-binary", default="app/rk3588_playout_rx/build/file_playout_rx")
    ap.add_argument("--duration", type=int, default=60)
    ap.add_argument("--udp", type=int, default=15000)
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--out", default=None)
    ap.add_argument("--port", default=None)
    args = ap.parse_args()
    th = load_thresholds()

    rx_cmd = [args.rx_binary, str(args.duration), "--udp", str(args.udp),
              "--sip", "127.0.0.1", "--width", str(args.width),
              "--height", str(args.height), "--fps", str(args.fps)]
    tx_cmd = [args.tx_binary, str(args.duration), "--udp", str(args.udp),
              "--dip", "127.0.0.1"]
    if args.port:
        rx_cmd += ["--port", args.port]
        tx_cmd += ["--port", args.port]
    print("run RX: " + " ".join(rx_cmd))
    rx = subprocess.Popen(rx_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(2)
    print("run TX: " + " ".join(tx_cmd))
    tx = subprocess.Popen(tx_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        try:
            tx_out = tx.communicate(timeout=args.duration + 60)[0]
        except subprocess.TimeoutExpired:
            tx.kill()
            tx_out = tx.communicate()[0]
        try:
            rx_out = rx.communicate(timeout=60)[0]
        except subprocess.TimeoutExpired:
            rx.kill()
            rx_out = rx.communicate()[0]
    except KeyboardInterrupt:
        print("\n[INT] 中断，停 TX/RX 后退出…")
        for p in (tx, rx):
            try:
                p.terminate()
            except Exception:
                pass
        import time as _t
        _t.sleep(5)
        for p in (tx, rx):
            if p.poll() is None:
                try:
                    p.kill()
                except Exception:
                    pass
        raise SystemExit(130)

    tx_rows = [r for line in tx_out.splitlines()
               for r in [parse_tx_line(line)] if r]
    _, totals, e2es = parse_rx_log(rx_out)
    if not tx_rows or not totals:
        print("[FAIL] 日志未解析（TX 行 {}，RX total {}）".format(len(tx_rows), len(totals)))
        return 1

    def mean(k):
        return round(sum(r[k] for r in tx_rows) / len(tx_rows), 1)

    cap_ms = round(mean("cap_int_us") / 1000, 2)
    enc_ms = round(mean("enc_cost_us") / 1000, 2)
    enc_p99_ms = round(max(r["enc_p99"] for r in tx_rows) / 1000, 2)
    rga_ms = round(mean("rga_ema_us") / 1000, 2)
    tx_typ = round(cap_ms + rga_ms + enc_ms, 2)
    tx_worst = round(cap_ms + rga_ms + enc_p99_ms, 2)
    e2e = e2es[-1] if e2es else None
    lo, hi = th.get("budget_ms", [70, 170])

    verdict, notes = "PASS", []
    if e2e:
        if e2e["skew"] > 0:
            verdict, notes = "FAIL", ["时钟偏斜，本轮 E2E 作废"]
        elif not (lo <= e2e["avg_ms"] <= hi):
            verdict, notes = "WARN", ["E2E-C avg {:.1f}ms 落在包络外".format(e2e["avg_ms"])]
        else:
            notes = ["E2E-C avg {:.1f}ms，包络内".format(e2e["avg_ms"])]
    else:
        verdict, notes = "WARN", ["无 E2E-C 样本（检查 SEI 戳与呈现路径）"]

    result = {
        "tool": "e2e_latency_budget",
        "tx_typical_ms": {"capture": cap_ms, "rga": rga_ms, "encode": enc_ms, "sum": tx_typ},
        "tx_worst_ms": {"capture": cap_ms, "rga": rga_ms, "encode_p99": enc_p99_ms, "sum": tx_worst},
        "e2e_measured": e2e,
        "budget_envelope_ms": [lo, hi],
        "verdict": verdict,
        "notes": notes,
    }
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "e2e_latency.json"), "w") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        write_summary(
            os.path.join(args.out, "e2e_latency.md"), "E2E 延迟预算",
            meta={"回环": "单板 127.0.0.1",
                  "结论": verdict + ("：" + "；".join(notes) if notes else "")},
            table_header=["段", "典型(ms)", "最坏(ms)"],
            table_rows=[["采集", cap_ms, cap_ms],
                        ["RGA", rga_ms, rga_ms],
                        ["编码", enc_ms, enc_p99_ms],
                        ["TX 合计", tx_typ, tx_worst],
                        ["E2E-C 实测 avg", (e2e or {}).get("avg_ms", "-"),
                         (e2e or {}).get("max_ms", "-")],
                        ["预算包络", lo, hi]])
        print("written: " + args.out)
    return 0 if verdict != "FAIL" else 1


if __name__ == "__main__":
    raise SystemExit(main())
