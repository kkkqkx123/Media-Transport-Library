#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MPP 编解码异步吞吐上限：sync/async 对比 + 硬件负载分类。

sync 复用探针默认口径（提交等待，量往返延迟）；async 走 feeder/collector
双线程流水（量硬件吞吐上限）。判定阈值见 common/thresholds.json 的
async_ceiling 段；设计见 docs/tests/mpp-async-ceiling-verification-design.md。

板端执行（只依赖标准库 + 板上 gcc 编出的探针）：
  python3 tests/perf/mpp_async_ceiling.py --matrix "1920x1080,3840x2160" \\
      --repeat 2 --out ~/code/mtl/perf/async_ceiling
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "common"))
from hw_load_sample import read_mpp_load, read_rkrga_load, summarize  # noqa: E402
from report import write_summary  # noqa: E402

_ENC_RESULT_RE = re.compile(
    r"RESULT async enc (\d+)x(\d+) frames=(\d+) wall_ms=([\d.]+) "
    r"wall_fps=([\d.]+) mean_us=([\d.]+) p50=(\d+) p90=(\d+) p99=(\d+) "
    r"out_kbps=([\d.]+) put_retries=(\d+)")
_ENC_SYNC_RE = re.compile(
    r"frames=(\d+) (\d+)x(\d+)@(\d+)fps level=(\d+) mean_us=([\d.]+) "
    r"p50=(\d+) p90=(\d+) p99=(\d+) out_kbps=([\d.]+)")
_DEC_RESULT_RE = re.compile(
    r"RESULT async dec au_fed=(\d+) framed=(\d+) no_output=(\d+) "
    r"put_fail=(\d+) wall_ms=([\d.]+) wall_fps=([\d.]+) err_frames=(\d+)")
_DEC_SYNC_RE = re.compile(
    r"au_fed=(\d+) framed=(\d+) no_output=(\d+) put_fail=(\d+)\s+"
    r"mean_us=([\d.]+) p50=(\d+) p90=(\d+) p99=(\d+)")


def load_thresholds():
    try:
        with open(os.path.join(_HERE, "common", "thresholds.json")) as f:
            return json.load(f).get("async_ceiling", {})
    except Exception:
        return {}


def compile_probe(src, out):
    """板端 gcc 直编探针；已存在且不强制重建则跳过。"""
    if os.path.exists(out):
        print("复用探针: " + out)
        return True
    cmd = ["gcc", "-O2", src, "-o", out, "-lrockchip_mpp",
           "-I/usr/include/rockchip", "-lpthread"]
    print("编译: " + " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if r.returncode != 0:
        print("[FAIL] 编译失败:\n" + r.stderr[-2000:])
        return False
    print("编译成功: " + out)
    return True


def run_with_load(cmd, interval=0.2):
    """跑命令的同时后台采样硬件负载；返回 (rc, 输出文本, 负载行)。"""
    try:
        with open("/proc/mpp_service/load_interval", "w") as f:
            f.write(str(int(interval * 1000)))
    except Exception:
        pass
    loads, stop = [], {"v": False}

    def _bg():
        while not stop["v"]:
            loads.append({"mpp": read_mpp_load(),
                          "rkrga": read_rkrga_load()})
            time.sleep(interval)

    t = threading.Thread(target=_bg, daemon=True)
    t.start()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        rc, text = r.returncode, r.stdout + r.stderr
    except Exception as ex:
        rc, text = -1, "run failed: {}".format(ex)
    finally:
        stop["v"] = True
        t.join(timeout=3)
    return rc, text, loads


def core_max_pct(loads, prefix):
    """某类核心（rkvenc/rkvdec）在整轮中的 load_pct 最大值。"""
    vals = []
    for row in loads:
        mpp = row.get("mpp") or {}
        for core, v in mpp.items():
            if core.startswith(prefix) and v and "load_pct" in v:
                vals.append(v["load_pct"])
    return max(vals) if vals else None


def run_enc(bin_path, nv12, w, h, frames, bps, fps, mode, warmup, src="file",
            csv_path=None):
    cmd = [bin_path, "--input", nv12, "--w", str(w), "--h", str(h),
           "--frames", str(frames), "--bps", str(bps), "--fps", str(fps),
           "--mode", mode, "--warmup", str(warmup), "--src", src]
    if csv_path:
        cmd += ["--csv", csv_path]
    print("run: " + " ".join(cmd))
    rc, text, loads = run_with_load(cmd)
    res = {"rc": rc, "mode": mode, "src": src, "w": w, "h": h,
           "fps_target": fps,
           "load": summarize(loads) if loads else {},
           "rkvenc_max": core_max_pct(loads, "rkvenc")}
    if mode == "async":
        m = _ENC_RESULT_RE.search(text)
        if rc == 0 and m:
            res.update({
                "frames_done": int(m.group(3)), "wall_ms": float(m.group(4)),
                "fps": float(m.group(5)), "mean_us": float(m.group(6)),
                "p50": int(m.group(7)), "p90": int(m.group(8)),
                "p99": int(m.group(9)), "put_retries": int(m.group(11)),
            })
            # 同步口径行（探针同时打印）提供 sojourn 分位，供中位数/报告用。
            ms = _ENC_SYNC_RE.search(text)
            if ms:
                res.update({
                    "mean_us": float(ms.group(6)),
                    "p50": int(ms.group(7)), "p90": int(ms.group(8)),
                    "p99": int(ms.group(9)),
                })
        else:
            res["error"] = "async 输出未解析 tail={}".format(text[-500:])
    else:
        m = _ENC_SYNC_RE.search(text)
        if rc == 0 and m:
            mean = float(m.group(6))
            res.update({
                "frames_done": int(m.group(1)), "mean_us": mean,
                "p50": int(m.group(7)), "p90": int(m.group(8)),
                "p99": int(m.group(9)),
                # 同步口径无 wall 计时：稳态吞吐 = 1e6/mean（不含进程启停）。
                "fps": round(1e6 / mean, 1) if mean > 0 else 0,
            })
        else:
            res["error"] = "sync 输出未解析 tail={}".format(text[-500:])
    return res


def parse_dec_output(text, rc, mode):
    """解析单次解码探针输出；并发批跑与 run_dec 共用。"""
    res = {"rc": rc, "mode": mode}
    if mode == "async":
        m = _DEC_RESULT_RE.search(text)
        if rc == 0 and m:
            res.update({
                "au_fed": int(m.group(1)), "frames": int(m.group(2)),
                "no_output": int(m.group(3)), "put_fail": int(m.group(4)),
                "wall_ms": float(m.group(5)), "fps": float(m.group(6)),
                "err_frames": int(m.group(7)),
            })
            ms = _DEC_SYNC_RE.search(text)
            if ms:
                res.update({
                    "mean_us": float(ms.group(5)),
                    "p50": int(ms.group(6)), "p90": int(ms.group(7)),
                    "p99": int(ms.group(8)),
                })
        else:
            res["error"] = "async 输出未解析 tail={}".format(text[-500:])
    else:
        m = _DEC_SYNC_RE.search(text)
        if rc == 0 and m:
            mean = float(m.group(5))
            res.update({
                "au_fed": int(m.group(1)), "frames": int(m.group(2)),
                "no_output": int(m.group(3)), "put_fail": int(m.group(4)),
                "mean_us": mean,
                "fps": round(1e6 / mean, 1) if mean > 0 else 0,
            })
        else:
            res["error"] = "sync 输出未解析 tail={}".format(text[-500:])
    return res


def run_dec(bin_path, h264, max_au, mode, warmup, duration_hint=60,
            depth=16):
    cmd = [bin_path, "--input", h264, "--mode", mode,
           "--warmup", str(warmup)]
    if max_au:
        cmd += ["--max-au", str(max_au)]
    if mode == "async":
        cmd += ["--depth", str(depth)]
    print("run: " + " ".join(cmd))
    rc, text, loads = run_with_load(cmd)
    res = parse_dec_output(text, rc, mode)
    res.update({"depth": depth if mode == "async" else None,
                "load": summarize(loads) if loads else {},
                "rkvdec_max": core_max_pct(loads, "rkvdec")})
    return res


def run_dec_batch(cmds, interval=0.2, timeout=900):
    """并发跑 N 个解码会话（多流 stretch 测量）；负载采样覆盖整批。"""
    try:
        with open("/proc/mpp_service/load_interval", "w") as f:
            f.write(str(int(interval * 1000)))
    except Exception:
        pass
    loads, stop = [], {"v": False}

    def _bg():
        while not stop["v"]:
            loads.append({"mpp": read_mpp_load(),
                          "rkrga": read_rkrga_load()})
            time.sleep(interval)

    t = threading.Thread(target=_bg, daemon=True)
    t.start()
    procs = []
    try:
        for cmd in cmds:
            print("batch run: " + " ".join(cmd))
            procs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT,
                                           text=True))
        outs = []
        for p in procs:
            try:
                out, _ = p.communicate(timeout=timeout)
                outs.append((p.returncode or 0, out))
            except Exception as ex:
                p.kill()
                outs.append((-1, "batch run failed: {}".format(ex)))
    finally:
        stop["v"] = True
        t.join(timeout=3)
    return outs, loads


def classify(fps_ratio_ok, load_max, th):
    if load_max is not None and load_max >= th.get("saturation_pct", 90.0):
        return "HW_CEILING"
    return "PIPELINE_THROUGHPUT"


def judge_enc(res, th, fps_target):
    notes = []
    if "error" in res:
        return "FAIL", [res["error"]]
    ratio = res["fps"] / fps_target if fps_target else 0
    res["fps_ratio"] = round(ratio, 3)
    verdict = "PASS" if ratio >= th.get("enc_fps_ratio", 0.95) else "FAIL"
    if verdict == "FAIL":
        notes.append("吞吐 {:.0%} 未达目标".format(ratio))
    res["class"] = classify(verdict == "PASS", res.get("rkvenc_max"), th)
    if res["class"] == "HW_CEILING":
        notes.append("rkvenc max {:.0f}% 饱和：该 fps 即单会话硬件上限".format(
            res.get("rkvenc_max") or 0))
    if res.get("put_retries"):
        notes.append("put_retries={}".format(res["put_retries"]))
    if not notes:
        notes = ["async fps={}（目标{}），{:.0%}".format(
            res["fps"], fps_target, ratio)]
    return verdict, notes


def judge_dec(res, th, fps_target, fps_ratio_key="dec_fps_ratio"):
    notes = []
    if "error" in res:
        return "FAIL", [res["error"]]
    verdict = "PASS"
    if res.get("put_fail", 0) > 0:
        verdict = "FAIL"
        notes.append("解码反压 put_fail={}".format(res["put_fail"]))
    if res.get("no_output", 0) > th.get("no_output_allowed", 12):
        verdict = "FAIL"
        notes.append("无输出 AU {} 过多".format(res["no_output"]))
    ratio = res.get("fps", 0) / fps_target if fps_target else 0
    res["fps_ratio"] = round(ratio, 3)
    if ratio < th.get(fps_ratio_key, 0.95):
        verdict = "FAIL"
        notes.append("吞吐 {:.0%} 未达目标".format(ratio))
    res["class"] = classify(verdict == "PASS", res.get("rkvdec_max"), th)
    if res["class"] == "HW_CEILING":
        notes.append("rkvdec max {:.0f}% 饱和：该 fps 即单会话硬件上限".format(
            res.get("rkvdec_max") or 0))
    if not notes:
        notes = ["async fps={}（目标{}），{:.0%}".format(
            res["fps"], fps_target, ratio)]
    return verdict, notes


def median_async(runs):
    ok = [r for r in runs if r.get("fps") is not None]
    if not ok:
        # Copy, never the element itself: callers attach all_runs, which
        # would self-reference (circular) if it were runs[0].
        return dict(runs[0]) if runs else {}
    med = dict(ok[0])
    for k in ("fps", "wall_ms", "mean_us"):
        vals = [r[k] for r in ok if k in r and r[k] is not None]
        if vals:
            med[k] = round(statistics.median(vals), 2)
    for k in ("p50", "p90", "p99"):
        vals = [r[k] for r in ok if k in r]
        if vals:
            med[k] = int(statistics.median(vals))
    return med


def guess_inputs(streams, w, h):
    tag = "{}x{}".format(w, h)
    if w == 1280 and h == 720:
        enc_cands = ["clock720_1280x720.nv12"]
        dec_cands = ["clock720_1280x720.h264"]
    else:
        enc_cands = ["clock_{}.nv12".format(tag)]
        dec_cands = ["clock_{}.h264".format(tag)]
    enc = next((os.path.join(streams, c) for c in enc_cands
                if os.path.exists(os.path.join(streams, c))), None)
    dec = next((os.path.join(streams, c) for c in dec_cands
                if os.path.exists(os.path.join(streams, c))), None)
    return enc, dec


def main():
    ap = argparse.ArgumentParser(description="MPP 异步吞吐上限对比 (板端执行)")
    ap.add_argument("--matrix", default="1920x1080",
                    help="逗号分隔 WxH 列表")
    ap.add_argument("--suites", default="enc,dec",
                    help="enc,dec 任选其一或都跑")
    ap.add_argument("--modes", default="compare",
                    choices=["compare", "async", "sync"])
    ap.add_argument("--frames", type=int, default=0,
                    help="编码帧数（0=按分辨率默认：1080p 120 / 4K 60）")
    ap.add_argument("--fps", type=float, default=0,
                    help="目标 fps（0=按分辨率默认：1080p 60 / 4K 30）")
    ap.add_argument("--bps", type=int, default=0,
                    help="编码码率（0=按分辨率默认：1080p 8M / 4K 40M）")
    ap.add_argument("--max-au", type=int, default=0)
    ap.add_argument("--srcs", default="file",
                    help="编码输入源列表：file（CPU 拷贝）/dmaheap（零拷贝直投），逗号分隔")
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--repeat", type=int, default=2,
                    help="每档 async 重复次数（取中位数）")
    ap.add_argument("--depths", default="16",
                    help="解码 async 门控深度扫点，逗号分隔（默认16=现行；"
                    " sweep 只记录不判定）")
    ap.add_argument("--concurrent", default="1",
                    help="解码并发会话数扫点，逗号分隔（如 1,2,4）；>1 时"
                    "同起 N 个会话测每流 stretch，只记录不判定")
    ap.add_argument("--cdepth", type=int, default=4,
                    help="并发批跑用的门控深度（默认4，即预期拐点；"
                    " sweep 后若拐点他处可重设）")
    ap.add_argument("--streams", default=os.path.expanduser("~/code/mtl/streams"))
    ap.add_argument("--src-dir", default=None,
                    help="探针源码目录（默认相对仓库 debug/）")
    ap.add_argument("--bin-dir", default=os.path.expanduser("~/code/mtl/bin"))
    ap.add_argument("--rebuild", action="store_true", help="强制重编探针")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    repo_debug = os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(_HERE))), "debug")
    src_dir = args.src_dir or repo_debug
    th = load_thresholds()

    enc_bin = os.path.join(args.bin_dir, "mpp_enc_latency")
    dec_bin = os.path.join(args.bin_dir, "mpp_dec_latency")
    if args.rebuild:
        for p in (enc_bin, dec_bin):
            if os.path.exists(p):
                os.remove(p)
    suites = [s.strip() for s in args.suites.split(",") if s.strip()]
    if "enc" in suites and not compile_probe(
            os.path.join(src_dir, "mpp_enc_frame_latency.c"), enc_bin):
        return 1
    if "dec" in suites and not compile_probe(
            os.path.join(src_dir, "mpp_dec_frame_latency.c"), dec_bin):
        return 1

    res_list = [(int(a), int(b)) for a, b in
                (item.split("x") for item in args.matrix.split(",") if item)]
    results, overall = [], "PASS"
    for w, h in res_list:
        is_4k = w > 1920 or h > 1080
        fps = args.fps or (30 if is_4k else 60)
        bps = args.bps or (40000000 if is_4k else 8000000)
        frames = args.frames or (60 if is_4k else 120)
        enc_in, dec_in = guess_inputs(args.streams, w, h)
        case = {"resolution": "{}x{}".format(w, h), "fps_target": fps}
        if "enc" in suites:
            if not enc_in:
                case["enc"] = {"verdict": "FAIL",
                               "notes": ["缺少 NV12 输入（{}）".format(args.streams)]}
                overall = "FAIL"
            else:
                enc_runs = {}
                modes = ["sync", "async"] if args.modes == "compare" \
                    else [args.modes]
                srcs = [s.strip() for s in args.srcs.split(",") if s.strip()]
                for src in srcs:
                    for m in modes:
                        key = "{}:{}".format(src, m) if len(srcs) > 1 else m
                        if m == "async":
                            runs = [run_enc(enc_bin, enc_in, w, h, frames, bps,
                                            fps, m, args.warmup, src)
                                    for _ in range(args.repeat)]
                            r = median_async(runs)
                            r["all_runs"] = runs
                        else:
                            r = run_enc(enc_bin, enc_in, w, h, frames, bps,
                                        fps, m, args.warmup, src)
                        v, notes = judge_enc(r, th, fps)
                        r["verdict"], r["notes"] = v, notes
                        enc_runs[key] = r
                        # Encode ceiling is judged by sync (1/service is the
                        # single-session ceiling; MPP encode is serial).
                        # Async-enc is diagnostic only: this MPP build emits
                        # stochastic multi-second output gaps that trip the
                        # gate regardless of depth, so async FAIL must not
                        # fail the run. Drops/timeout healing keeps its data
                        # honest; see design doc section 6.
                        if m == "sync" and v == "FAIL":
                            overall = "FAIL"
                    sk, ak = ("{}:sync", "{}:async") if len(srcs) > 1 \
                        else ("sync", "async")
                    sk, ak = sk.format(src), ak.format(src)
                    if sk in enc_runs and ak in enc_runs:
                        s = enc_runs[sk].get("fps")
                        a = enc_runs[ak].get("fps")
                        if s and a:
                            enc_runs["{}:async_vs_sync".format(src)] = \
                                round(a / s, 2)
                case["enc"] = enc_runs
        if "dec" in suites:
            if not dec_in:
                case["dec"] = {"verdict": "FAIL",
                               "notes": ["缺少 H264 输入（{}）".format(args.streams)]}
                overall = "FAIL"
            else:
                dec_runs = {}
                modes = ["sync", "async"] if args.modes == "compare" \
                    else [args.modes]
                for m in modes:
                    if m == "async":
                        runs = [run_dec(dec_bin, dec_in, args.max_au, m,
                                        args.warmup)
                                for _ in range(args.repeat)]
                        r = median_async(runs)
                        r["all_runs"] = runs
                    else:
                        r = run_dec(dec_bin, dec_in, args.max_au, m, args.warmup)
                    v, notes = judge_dec(r, th, fps)
                    r["verdict"], r["notes"] = v, notes
                    dec_runs[m] = r
                    if m == "async" and v == "FAIL":
                        overall = "FAIL"
                if "sync" in dec_runs and "async" in dec_runs:
                    s, a = dec_runs["sync"].get("fps"), dec_runs["async"].get("fps")
                    if s and a:
                        dec_runs["async_vs_sync"] = round(a / s, 2)
                # Depth sweep（单会话门控深度拐点）与并发 stretch 均为表征，
                # 只记录 INFO，不翻转 overall（探针报错除外）。
                if "async" in modes:
                    depths = []
                    for d in args.depths.split(","):
                        d = d.strip()
                        if d:
                            depths.append(int(d))
                    sweep = {}
                    for d in depths:
                        runs = [run_dec(dec_bin, dec_in, args.max_au, "async",
                                       args.warmup, depth=d)
                                for _ in range(args.repeat)]
                        r = median_async(runs)
                        r["all_runs"] = runs
                        if r.get("fps") and fps:
                            r["fps_ratio"] = round(r["fps"] / fps, 3)
                        if "error" in r:
                            # Functional unavailability at this depth (gate
                            # timeout etc., rc=1) is characterization data
                            # (min viable depth); only abnormal exit (crash)
                            # fails the run.
                            if r.get("rc") not in (0, 1):
                                r["verdict"], r["notes"] = "FAIL", [r["error"]]
                                overall = "FAIL"
                            else:
                                r["verdict"] = "INFO"
                                r["notes"] = ["depth={} 不可用 rc={}".format(
                                    d, r.get("rc"))]
                        else:
                            r["verdict"] = "INFO"
                            r["notes"] = [
                                "depth={} 单会话表征 fps={}".format(d, r.get("fps"))]
                        sweep[str(d)] = r
                    dec_runs["depth_sweep"] = sweep
                    conc = []
                    for n in args.concurrent.split(","):
                        n = n.strip()
                        if n:
                            conc.append(int(n))
                    conc_res = {}
                    for n in conc:
                        if n <= 1:
                            continue
                        cmds = []
                        for _ in range(n):
                            cmd = [dec_bin, "--input", dec_in, "--mode",
                                   "async", "--warmup", str(args.warmup),
                                   "--depth", str(args.cdepth)]
                            if args.max_au:
                                cmd += ["--max-au", str(args.max_au)]
                            cmds.append(cmd)
                        outs, cloads = run_dec_batch(cmds)
                        sessions = []
                        for rc, text in outs:
                            sessions.append(parse_dec_output(text, rc, "async"))
                        ok = [s for s in sessions if s.get("fps") is not None]
                        batch = {"sessions": n, "cdepth": args.cdepth,
                                 "per_session": sessions,
                                 "rkvdec_max": core_max_pct(cloads, "rkvdec")}
                        if len(ok) != n:
                            batch["verdict"] = "FAIL"
                            batch["notes"] = ["{} / {} 会话无输出".format(
                                n - len(ok), n)]
                            overall = "FAIL"
                        else:
                            fpss = [s["fps"] for s in ok]
                            batch["min_fps"] = round(min(fpss), 1)
                            batch["mean_fps"] = round(
                                sum(fpss) / len(fpss), 1)
                            ref = sweep.get(str(args.cdepth), {}).get("fps")
                            ref = ref or dec_runs.get("async", {}).get("fps")
                            batch["ref_single_fps"] = ref
                            batch["stretch"] = round(batch["min_fps"] / ref, 3) \
                                if ref else None
                            batch["verdict"] = "INFO"
                            batch["notes"] = [
                                "x{} 并发@depth={} min_fps={} stretch={}".format(
                                    n, args.cdepth, batch["min_fps"],
                                    batch["stretch"])]
                        conc_res[str(n)] = batch
                    if conc_res:
                        dec_runs["concurrent"] = conc_res
                case["dec"] = dec_runs
        results.append(case)

    out = {"tool": "mpp_async_ceiling", "args": vars(args),
           "verdict": overall, "results": results}
    print(json.dumps(out, ensure_ascii=False, indent=2))
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "mpp_async_ceiling.json"), "w") as f:
            json.dump(out, f, ensure_ascii=False, indent=2)
        rows = []
        for c in results:
            for suite in ("enc", "dec"):
                runs = c.get(suite) or {}
                for key, r in runs.items():
                    if not isinstance(r, dict):
                        continue
                    if key == "depth_sweep":
                        for d, dr in r.items():
                            if not isinstance(dr, dict) or "fps" not in dr:
                                continue
                            rows.append([c["resolution"], suite,
                                         "async-depth-{}".format(d),
                                         dr.get("fps"),
                                         dr.get("fps_ratio", "-"),
                                         dr.get("rkvdec_max", "-"), "-",
                                         dr.get("verdict", "?")])
                        continue
                    if key == "concurrent":
                        for n, b in r.items():
                            if not isinstance(b, dict):
                                continue
                            fps = b.get("min_fps")
                            tgt = c.get("fps_target") or 0
                            ratio = round(fps / tgt, 3) if fps and tgt else "-"
                            rows.append([c["resolution"], suite,
                                         "async-x{}".format(n), fps, ratio,
                                         b.get("rkvdec_max", "-"), "-",
                                         b.get("verdict", "?")])
                        continue
                    if "fps" not in r:
                        continue
                    rows.append([c["resolution"], suite, key, r.get("fps"),
                                 r.get("fps_ratio", "-"),
                                 r.get("rkvenc_max", r.get("rkvdec_max", "-")),
                                 r.get("class", "-"), r.get("verdict", "?")])
        write_summary(
            os.path.join(args.out, "mpp_async_ceiling.md"), "MPP 异步吞吐上限",
            meta={"矩阵": args.matrix, "结论": overall},
            table_header=["分辨率", "套件", "模式", "fps", "fps比",
                          "硬件max(%)", "分类", "结论"],
            table_rows=rows)
        print("written: " + args.out)
    return 0 if overall != "FAIL" else 1


if __name__ == "__main__":
    raise SystemExit(main())
