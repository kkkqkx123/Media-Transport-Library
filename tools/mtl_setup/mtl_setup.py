#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""
MTL host setup CLI — stdlib-only successor of the former `mtl-system-setup`
and `mtl-acceptance-setup` MCP servers that lived under .github/mcp/.

Prepares a Linux host to run the Media Transport Library (SMPTE ST 2110 over
DPDK): hugepages, NIC/VF management, ICE driver, DPDK, MTL builds,
MtlManager, and integration gtests. The `acceptance-*` command group takes a
host to "ready to run tests/acceptance/ pytest" on its separate
.local_install tree. All setup logic still lives in the standalone
.github/scripts/ (and script/) shell scripts — these commands are a thin,
validating wrapper, so a human can run the exact same steps by hand.

Usage (no venv, no pip — plain python3 >= 3.10; run from the repo root):
    python3 tools/mtl_setup/mtl_setup.py --help
    python3 tools/mtl_setup/mtl_setup.py <command> [flags]
    sudo python3 tools/mtl_setup/mtl_setup.py setup-after-reboot-auto
    sudo python3 tools/mtl_setup/mtl_setup.py run-gtest --gtest-filter 'St20p*'
    sudo python3 tools/mtl_setup/mtl_setup.py acceptance-setup-tests-full \\
        --nfs-source 10.0.0.5:/mnt/media --pf-bdf 0000:c9:00.0

Common workflows:
    First-time setup:   system-status → iommu-status → hugepages-set →
                        nic-create-vf → build-mtl → manager-start
    After reboot:       setup-after-reboot-auto (single step — auto-rebuilds
                        the ICE driver first if needed, e.g. after a kernel
                        upgrade, then hugepages, VFs, and MtlManager)
    Force ICE rebuild:  ice-driver-rebuild → setup-after-reboot-auto
                        (VFs are destroyed by driver reload!)
    Run tests:          run-gtest (auto-discovers ports);
                        run-noctx-tests / run-noctx-pf-tests for NoCtxTest
                        cases (each needs its own KahawaiTest process)
    Build from clean:   mtl-clean-rebuild
    Switch PF→VF:       nic-bind-kernel → nic-create-vf
    Switch VF→PF:       nic-destroy-vf → nic-bind-pmd
    Debug crash:        dmesg-tail → ice-driver-status → dpdk-status →
                        log-tail
    Acceptance pytest:  acceptance-discover-status → acceptance-setup-tests-full

Long build/test commands stream their output to build/logs/<name>_<ts>.log
while running — start them in a background shell and tail the log to watch
progress. Exit codes: 0 success, 1 a command step failed (the printed log
path has the details), 2 invalid input.
"""

from __future__ import annotations

import argparse
import inspect
import re
import sys
from pathlib import Path
from typing import Callable

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import mtl_setup_common  # noqa: E402  (registered lazily for the exit code)
import acceptance_cmds  # noqa: E402
import system_cmds  # noqa: E402

_ARG_KEY_RE = re.compile(r"^([a-z][a-z0-9_]*):\s+(.*)$")
_SECTION_END_RE = re.compile(r"^[A-Z][A-Za-z]*:$")
_TOOL_TYPES = ("str", "int", "bool")


def _first_line(doc: str | None) -> str:
    """One-line summary = first non-empty line of the tool docstring."""
    for line in (doc or "").strip().splitlines():
        if line.strip():
            return line.strip()
    return ""


def _parse_args_section(doc: str | None) -> dict[str, str]:
    """Extract `name: description` entries from the docstring "Args:" block.

    Multi-line entries are merged onto their key. Returns an empty dict when
    a docstring has no Args section — help text is best-effort, never fatal.
    """
    result: dict[str, str] = {}
    current: str | None = None
    in_args = False
    for line in (doc or "").splitlines():
        stripped = line.strip()
        if not in_args:
            if stripped == "Args:":
                in_args = True
            continue
        if not stripped:
            continue
        if _SECTION_END_RE.match(stripped):  # Returns:/Raises:/... ends Args
            break
        m = _ARG_KEY_RE.match(stripped)
        if m:
            current = m.group(1)
            result[current] = m.group(2)
        elif current:
            result[current] = f"{result[current]} {stripped}"
    return result


def _tools_from(module, prefix: str = "") -> dict[str, Callable[..., str]]:
    """Collect a module's tool functions for argparse registration.

    A tool is a public function defined in `module` itself that returns
    `str` and takes only str/int/bool positional-or-keyword parameters —
    exactly the shape both command groups keep (helpers are all
    underscore-prefixed). Names are kebab-cased; `prefix` groups them (the
    acceptance commands run as `acceptance-*` to avoid ambiguous verbs like
    bare `discover-status`).
    """
    tools: dict[str, Callable[..., str]] = {}
    for name, fn in vars(module).items():
        if name.startswith("_"):
            continue
        if getattr(fn, "__module__", None) != module.__name__:
            continue
        if not inspect.isfunction(fn) or fn.__annotations__.get("return") != "str":
            continue
        params = inspect.signature(fn).parameters
        if any(
            p.kind is not p.POSITIONAL_OR_KEYWORD or p.annotation not in _TOOL_TYPES
            for p in params.values()
        ):
            continue
        tools[prefix + name.replace("_", "-")] = fn
    return tools


def _add_arguments(sp: argparse.ArgumentParser, fn: Callable[..., str]) -> None:
    """Map a tool's parameters onto flags (or positionals when required).

    bool=False -> `--flag` store_true; bool=True -> `--no-flag` store_false
    (default stays on); int/str -> typed flag with the tool's own default.
    Parameter help comes from the docstring Args section.
    """
    defaults = _parse_args_section(fn.__doc__)
    for pname, param in inspect.signature(fn).parameters.items():
        kebab = pname.replace("_", "-")
        entry = defaults.get(pname) or pname
        if param.default is inspect.Parameter.empty:
            sp.add_argument(kebab, help=entry)
            continue
        flag = f"--{kebab}"
        if param.annotation == "bool" or isinstance(param.default, bool):
            if param.default:
                sp.add_argument(
                    f"--no-{kebab}",
                    dest=pname,
                    action="store_false",
                    default=True,
                    help=f"{entry} — pass --no-{kebab} to turn off (default: on)",
                )
            else:
                sp.add_argument(
                    flag,
                    dest=pname,
                    action="store_true",
                    default=False,
                    help=f"{entry} — pass {flag} to enable (default: off)",
                )
        elif param.annotation == "int" or isinstance(param.default, int):
            sp.add_argument(
                flag,
                dest=pname,
                type=int,
                default=param.default,
                help=f"{entry} (default: {param.default})",
            )
        else:
            sp.add_argument(
                flag,
                dest=pname,
                type=str,
                default=param.default,
                help=f"{entry} (default: {param.default!r})",
            )


def _build_parser() -> tuple[argparse.ArgumentParser, dict[str, Callable[..., str]]]:
    parser = argparse.ArgumentParser(
        prog="mtl_setup.py",
        description=_first_line(__doc__)
        + "\n\nFull usage, workflows and exit codes:\n"
        + inspect.cleandoc(__doc__ or ""),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command", metavar="COMMAND", required=True)

    tools: dict[str, Callable[..., str]] = {}
    tools.update(_tools_from(system_cmds))
    tools.update(_tools_from(acceptance_cmds, prefix="acceptance-"))

    for cmd, fn in tools.items():
        sp = sub.add_parser(
            cmd,
            help=_first_line(fn.__doc__),
            description=inspect.cleandoc(fn.__doc__ or cmd),
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        _add_arguments(sp, fn)
    return parser, tools


def main(argv: list[str] | None = None) -> int:
    parser, tools = _build_parser()
    args = parser.parse_args(argv)
    fn = tools[args.command]
    kwargs = {p: getattr(args, p) for p in inspect.signature(fn).parameters}
    result = fn(**kwargs)
    if result:
        print(result)
    if isinstance(result, str) and result.startswith("Error"):
        return 2
    return 1 if mtl_setup_common.LAST_FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
