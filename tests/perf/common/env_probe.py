#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""板端环境基线探测：每次测试必跑，版本不同则上限不可比。"""
import argparse
import json
import os
import re
import subprocess


def sh(cmd, timeout=10):
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return (r.stdout + r.stderr).strip()
    except Exception:
        return ""


def main():
    ap = argparse.ArgumentParser(description="RK3588 环境基线探测 (板端执行)")
    ap.add_argument("--out", default=None, help="JSON 输出路径")
    args = ap.parse_args()

    env = {}
    env["soc"] = sh("cat /proc/device-tree/compatible 2>/dev/null | tr '\\0' ' '").strip()
    env["kernel"] = sh("uname -r")
    env["mpp_version"] = sh("cat /proc/mpp_service/version 2>/dev/null | head -1")
    env["rkrga_load"] = sh("cat /sys/kernel/debug/rkrga/load 2>/dev/null | head -8")
    env["v4l2_fmt"] = sh("v4l2-ctl -d /dev/video0 --get-fmt-video 2>&1 | head -12")
    env["dv_timings"] = sh("v4l2-ctl -d /dev/video0 --query-dv-timings 2>&1 | head -12")
    env["governor"] = sh("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null")
    env["cpu_freq_max_khz"] = sh("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null")
    env["mem"] = sh("free -m | head -2")
    env["nproc"] = sh("nproc")
    env["mpi_enc"] = sh("ls -l /usr/bin/mpi_enc_test /usr/bin/mpi_dec_test 2>&1")
    # rkvenc core 列表：RK3588 通常双核，负载采样时逐核读取
    cores = []
    for line in sh("ls /proc/mpp_service/ 2>/dev/null").split():
        if line.startswith("rkvenc") or line.startswith("rkvdec"):
            cores.append(line)
    env["mpp_cores"] = cores
    print(json.dumps(env, ensure_ascii=False, indent=2))
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w") as f:
            json.dump(env, f, ensure_ascii=False, indent=2)
        print("written: " + args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
