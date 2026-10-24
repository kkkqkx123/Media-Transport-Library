#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""
Shared implementation helpers for the MTL host setup CLI (tools/mtl_setup).

Both command groups — `system_cmds.py` (system-wide host setup, driver
management, gtest) and `acceptance_cmds.py` (tests/acceptance/ pytest
environment, built on the separate `.local_install` tree) shell out to the
same `.github/scripts/` setup scripts and need the same output-summarization
behavior. Rather than duplicating this logic in each group, both import it
from here.

History: this module was shared by the `mtl-system-setup` and
`mtl-acceptance-setup` MCP servers; the MCP layer was removed so the tools
run with stdlib Python only (no venv, no pip installs).
"""

from __future__ import annotations

import os
import re
import subprocess  # nosec B404 # callers pass argv lists or fixed, validated commands
import threading
from datetime import datetime
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent  # tools/mtl_setup -> repo root
VERSIONS_ENV = REPO_ROOT / "versions.env"

# Set by _summarize_log() when a command reports a non-zero rc; the CLI entry
# point (mtl_setup.py) maps it to exit code 1, so callers can gate on the
# process exit status instead of parsing output text.
LAST_FAIL = False


# ---------------------------------------------------------------------------
# Process helpers
# ---------------------------------------------------------------------------
def _run(
    cmd: list[str] | str,
    *,
    sudo: bool = False,
    check: bool = True,
    timeout: int = 300,
    cwd: str | Path | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run a command and return CompletedProcess."""
    if isinstance(cmd, str):
        cmd = ["bash", "-c", cmd]
    if sudo:
        cmd = ["sudo"] + cmd
    merged_env = {**os.environ, **(env or {})}
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=check,
        cwd=cwd or REPO_ROOT,
        env=merged_env,
    )


def _decode(data: str | bytes | None) -> str:
    """Coerce subprocess output to str.

    subprocess.run(..., text=True) decodes stdout/stderr on normal return,
    but on a TimeoutExpired the partially-read buffers attached to the
    exception (e.stdout/e.stderr) can still be raw bytes regardless of
    text=True -- the decode step only runs on the happy path. Concatenating
    that bytes value with a str (as a naive '+' would) raises 'can't concat
    str to bytes', which masked the actual timeout with an unrelated-looking
    command error. Always decode explicitly before concatenating.
    """
    if data is None:
        return ""
    if isinstance(data, bytes):
        return data.decode(errors="replace")
    return data


def _run_rc(cmd: list[str] | str, **kw: Any) -> tuple[int, str]:
    """Run and return (returncode, combined stdout+stderr), never raising.

    On timeout, returncode is -1 and output includes a timeout marker.
    """
    try:
        r = _run(cmd, check=False, **kw)
        out = r.stdout
        if r.stderr:
            out += "\n" + r.stderr
        return r.returncode, out.strip()
    except subprocess.TimeoutExpired as e:
        out = _decode(e.stdout) + "\n" + _decode(e.stderr)
        out += f"\n\n*** TIMEOUT after {e.timeout}s ***"
        return -1, out.strip()


def _run_stream_to_log(
    name: str,
    cmd: list[str] | str,
    *,
    timeout: int = 600,
    cwd: str | Path | None = None,
    env: dict[str, str] | None = None,
    sudo: bool = False,
) -> tuple[int, str, Path]:
    """Run `cmd`, teeing stdout+stderr into build/logs/<name>_<ts>.log live.

    The `_run`-family only yields output when the process exits, so a
    multi-minute build or test run gives a caller watching the progress
    nothing until the end — and a caller killed by its own timeout loses the
    output entirely. This variant writes every line to the log file (and
    buffers it for the return value) as it is produced, so long commands can
    run in a background shell while the log is tailed. Returns
    (returncode, combined output, log path); on timeout the process is
    killed, rc is -1 (matching _run_rc) and the partial output is returned.
    """
    if isinstance(cmd, str):
        cmd = ["bash", "-c", cmd]
    if sudo:
        cmd = ["sudo"] + cmd
    merged_env = {**os.environ, **(env or {})}

    log_dir = REPO_ROOT / "build" / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = log_dir / f"{name}_{ts}.log"

    proc = subprocess.Popen(  # nosec B603 # argv lists / validated commands
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        cwd=cwd or REPO_ROOT,
        env=merged_env,
    )
    timed_out = threading.Event()
    watchdog = threading.Timer(timeout, lambda: (timed_out.set(), proc.kill()))
    watchdog.start()
    chunks: list[str] = []
    try:
        with open(log_path, "w", encoding="utf-8") as logf:
            for line in proc.stdout:
                chunks.append(line)
                logf.write(line)
                logf.flush()
        proc.stdout.close()
        rc = proc.wait()
    finally:
        watchdog.cancel()

    out = "".join(chunks)
    if timed_out.is_set():
        out += f"\n\n*** TIMEOUT after {timeout}s (process killed) ***\n"
        rc = -1
    return rc, out.strip(), log_path


def _run_output(cmd: list[str] | str, **kw: Any) -> str:
    """Run and return combined stdout+stderr, never raising on non-zero rc.

    Discards the return code — prefer `_run_rc` for anything whose caller
    needs to know pass/fail without grepping text.
    """
    _rc, out = _run_rc(cmd, **kw)
    return out


_XTRACE_RE = re.compile(r"^\s*\+")


def _strip_xtrace(text: str) -> str:
    """Drop `set -x` trace lines (e.g. from setup_environment.sh's `set -xe`).

    Scripts that trace every command emit ~5 noise lines per real line of
    output (`+ var=...`, `+ '[' 0 = 0 ']'`, `+ continue`, ...). A positional
    "last N lines" tail of that is almost always 100% trace noise from
    whatever loop happened to run last, not the actual build result.
    """
    return "\n".join(line for line in text.splitlines() if not _XTRACE_RE.match(line))


def _save_test_log(name: str, content: str) -> Path:
    """Save test output to a timestamped log file under build/logs/."""
    log_dir = REPO_ROOT / "build" / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = log_dir / f"{name}_{ts}.log"
    log_path.write_text(content)
    return log_path


def _summarize_log(
    log_path: Path | str, out: str, tail_lines: int = 40, rc: int | None = None
) -> str:
    """Build the pass/fail summary for output already saved in `log_path`.

    Same contract as _summarize_output() but without re-saving — used by the
    commands that streamed their output through _run_stream_to_log().
    """
    global LAST_FAIL
    total_lines = len(out.splitlines())
    header = f"- Full log: `{log_path}` ({total_lines} lines)"
    if rc is not None:
        if rc != 0:
            LAST_FAIL = True
        header = f"- **Result: {'OK' if rc == 0 else f'FAILED (exit {rc})'}**\n{header}"
    if rc == 0:
        return header
    clean = _strip_xtrace(out)
    tail = "\n".join(clean.splitlines()[-tail_lines:])
    return f"{header}\n### Output (last {tail_lines} non-trace lines)\n```\n{tail}\n```"


def _summarize_output(
    name: str, out: str, tail_lines: int = 40, rc: int | None = None
) -> str:
    """Save large command output to a log file, returning a short summary.

    Full build/install output can be thousands of lines — dumping it into a
    tool result burns context for little benefit. This saves it to disk and
    returns a clear pass/fail line (when `rc` is given) plus the log path and
    total line count, so the caller can grep/read the file directly if more
    is needed. The tail (xtrace-stripped) is included only on failure, or
    when `rc` is not given (legacy callers) — a successful step doesn't need
    its noisy tail pasted into every conversation that calls it.
    """
    return _summarize_log(
        _save_test_log(name, out), out, tail_lines=tail_lines, rc=rc
    )


def _load_versions() -> dict[str, str]:
    """Parse versions.env into a dict."""
    result: dict[str, str] = {}
    if VERSIONS_ENV.is_file():
        for line in VERSIONS_ENV.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, _, v = line.partition("=")
                # Expand simple ${VAR} references within the file
                for ref_k, ref_v in result.items():
                    v = v.replace(f"${{{ref_k}}}", ref_v)
                result[k.strip()] = v.strip()
    return result


# ---------------------------------------------------------------------------
# Shared business logic — called as plain functions by BOTH command groups,
# so neither duplicates the underlying setup_environment.sh invocation. Each
# group's own subcommand (if it exposes one of these directly) is a thin
# wrapper that just calls the matching function below.
#
# The actual host probing/mutation (ICE driver, hugepages, CPU governor, apt
# deps) lives in .github/scripts/lib/mtl_host_common.sh — a plain bash
# library also sourced directly by the acceptance_setup*.sh scripts, so a
# human running those scripts by hand and this CLI share exactly one
# implementation instead of a Python copy and a bash copy drifting apart.
# ---------------------------------------------------------------------------
HOST_COMMON_LIB = REPO_ROOT / ".github" / "scripts" / "lib" / "mtl_host_common.sh"


def _call_host_lib(func_call: str, **kw: Any) -> tuple[int, str]:
    """Source mtl_host_common.sh and call one of its mh_* functions."""
    return _run_rc(f"source {HOST_COMMON_LIB} && {func_call}", **kw)


def install_dependencies() -> str:
    """Install system build dependencies (apt packages) for MTL."""
    rc, out = _call_host_lib("mh_install_dependencies", timeout=300)
    return f"## Install Dependencies\n{_summarize_output('install_dependencies', out, rc=rc)}"


def ice_driver_status() -> str:
    """Check the ICE driver status: loaded version vs required, OOT or stock."""
    _rc, out = _call_host_lib("mh_ice_driver_status_report")
    return out


def ice_driver_rebuild() -> str:
    """Build and install the patched out-of-tree ICE driver, then reload it."""
    rc, out = _call_host_lib("mh_ice_driver_rebuild", timeout=600)

    return (
        f"## ICE Driver Build + Reload\n{_summarize_output('ice_driver_rebuild', out, rc=rc)}\n\n"
        "## ⚠ Important: VFs Destroyed\n"
        "The ICE driver reload destroyed all existing VFs.\n"
        "You MUST re-create VFs before running any tests:\n"
        "- Use `setup-after-reboot-auto` to re-create VFs on all PFs\n"
        "- Or use `nic-create-vf` for a specific PF\n"
        "- Then restart `manager-start` if it was running"
    )


def hugepages_get() -> str:
    """Get current hugepage status (total, free, size)."""
    _rc, out = _call_host_lib("mh_hugepages_report")
    info = "\n".join(
        out.splitlines()[1:]
    )  # drop the "## Hugepages" header, added below
    return f"## Hugepages\n```\n{info}\n```"


def hugepages_set(nr_hugepages: int = 2048, size_kb: int = 2048) -> str:
    """Configure hugepages. Default: 2048 x 2MB = 4GB."""
    rc, out = _call_host_lib(f"mh_hugepages_set {nr_hugepages} {size_kb}")
    if rc != 0:
        return f"Error: hugepage size {size_kb}kB not supported.\n{out}"

    total_mb = nr_hugepages * size_kb // 1024
    after = hugepages_get()
    return after.replace(
        "## Hugepages",
        f"## Hugepages\nSet {nr_hugepages} x {size_kb}kB hugepages ({total_mb} MB total).",
        1,
    )


def cpu_governor_set_and_confirm_performance() -> str:
    """Set all CPU scaling governors to performance and confirm status."""
    _rc, out = _call_host_lib("mh_cpu_governor_set_and_confirm_performance")
    return out
