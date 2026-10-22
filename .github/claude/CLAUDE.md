# Media Transport Library — Agent Guide

## Project intent and the RK3588 apps

This fork's goal is to run end-to-end ST 2110 media pipelines on the Rockchip RK3588 (ARM) using MTL as the transport layer. Upstream MTL targets Intel x86 + E810 NICs; the rk3588 apps prove the same stack on ARM with software pacing. Analysis lives in `docs/analysis/`.

Two standalone apps live on the `feat/rk3588-apps` branch (commit `add rk3568 app`), each with its own `meson.build` and **not** wired into `app/meson.build` — build them separately with `meson setup build && ninja -C build` inside their directory. They depend on Rockchip MPP (codec), RGA, DRM, V4L2, and ALSA.

| App | What it does |
|---|---|
| `app/rk3588_end2end/` | Low-latency live pipeline: V4L2 capture → MPP encode (+RGA conversion) → MTL TX → MTL RX → MPP decode → DRM render. `codec/` wraps MPP behind `codec_plugin.h`; `hal/` has DMA allocator, slab pool, timer, SEI timestamps; `pipeline/` has TX/RX pipelines, ring buffer, slice reassembler; `mtl_tx.c`/`mtl_rx.c`/`mtl_rx_bridge.c` wrap the MTL session layer. Unit tests in `tests/`. |
| `app/rk3588_mp4_dist/` | MP4 file distribution: TX demuxes MP4 (`tx/mp4_demux`, playlist, `tx_scheduler`) and sends video/audio over MTL; RX does playout buffering, AV sync, ALSA audio, multi-window composition (`rx/rx_compositor`) and DRM display. `shared/` duplicates MPP decode/DRM/ring/slab helpers from the other app (each app carries its own copy). Includes dual-host PTP configs (`config/ptp/`) and Python log-verification scripts in `tests/`. |

---

## Read first: the repo's agent knowledge base

Prefer these over re-deriving things from source:

| Doc | Use when |
|---|---|
| `.github/copilot-docs/mtl-knowledge-base.md` | Architecture reference (§1 design, §2 scheduler, §3 memory, §4 locking, §5 pacing, §6 session lifecycle, §7 DPDK patterns, §8 testing). Read the relevant § before any non-trivial library change. |
| `.github/instructions/mtl-c-coding.instructions.md` | Mandatory C rules. |
| `.github/instructions/mtl-gtest.instructions.md` | Running/debugging `KahawaiTest`, suite map, pacing modes, failure signatures. |
| `.github/instructions/mtl-acceptance-*.instructions.md` | pytest E2E: running, authoring, engine internals, host setup. |
| `.github/instructions/mtl-system-setup.instructions.md` | Hugepages, VFs, ICE driver, MtlManager. |
| `doc/design.md`, `doc/build.md`, `doc/run.md` | Upstream user-facing guides. |

**If the knowledge base disagrees with the code, fix the knowledge base in the same change.**

---

## Gotchas (read this first)

- **Two install trees, not interchangeable**: `build/` + `/usr/local` is what gtest/`KahawaiTest` uses; `.local_install/mtl/bin/{MtlManager,RxTxApp}` is what the pytest acceptance suite uses (`tests/acceptance/mtl_engine/const.py` hardcodes `PREFIX = ".local_install"`).
- **`tests/acceptance/mtl_engine/` contains BOTH `RxTxApp.py` and `rxtxapp.py` as real, different files.** On case-insensitive filesystems (Windows/macOS) git records edits to the wrong one. Never assume they are the same file.
- **Two near-duplicate modules**: `rx_compositor` vs the end2end `io/drm_renderer.c` and `mp4_dist/shared/drm_renderer.c`. Confirm the target file before editing.
- **`mt-system-admin`'s "MCP tools only, never a shell" rule is enforced by its prompt, not by withholding the Bash tool** (MCP wildcards in a subagent's `tools:` list aren't reliably supported). If you see it reach for Bash, that's a bug in the agent's behavior, not permission to allow it.

---

## Native tooling (Claude Code)

The Copilot workflow has been ported to Claude Code equivalents under `.github/claude/`. Claude Code only discovers config at fixed paths, so three tracked symlinks bridge the two:

| Discovery path | Real file |
|---|---|
| `CLAUDE.md` | `.github/claude/CLAUDE.md` |
| `.claude/` | `.github/claude/` (agents, skills, settings) |
| `.mcp.json` | `.github/claude/mcp.json` |

**Edit the file under `.github/claude/`, never the symlink.** Machine-local overrides go in `.github/claude/settings.local.json` (gitignored).

**Skills** (`.github/claude/skills/`): `mtl-build`, `mtl-write-test`, `mtl-commit`, each a symlink to `.github/skills/<name>/`.

**MCP servers** (`.github/claude/mcp.json`, enabled in `settings.json`): `mtl-system-setup` and `mtl-acceptance-setup`, backed by `.github/mcp/`. Their tools are deferred: **call `ToolSearch` before invoking `mcp__mtl-system-setup__*` etc.**

**Path-scoped context**: `lib/`, `tests/unit/`, `tests/integration_tests/`, and `tests/acceptance/` each carry a small `CLAUDE.md` that imports the matching `.github/instructions/*.md`, reproducing Copilot's `applyTo:` auto-attach.

---

## Build

```bash
./build.sh                  # release: lib -> app -> tests -> plugins -> ld_preload -> manager -> RxTxApp, each installed
./build.sh debug            # debug + ASan
./build.sh debugonly        # debug, no ASan
./build.sh unit             # configure build_unit/, build and RUN the unit gtest suite, then exit
./build.sh enable_fuzzing   # add libFuzzer harnesses
MTL_INSTALL_PREFIX=$PWD/.local_install ./build.sh   # local prefix install
```

Each subdirectory (`app/`, `tests/`, `plugins/`, `manager/`, `tests/tools/RxTxApp/`) is its own meson project. Library-only incremental rebuild: `ninja -C build`.

DPDK must be built and installed first (patched from `patches/`); pinned dependency versions live in `versions.env`.

---

## Format and lint

```bash
./format-coding.sh          # clang-format-14 (C/C++), isort+black (Python), shfmt (shell)
```

**`clang-format-14` specifically** — CI rejects output from other versions. Run before every commit. `.clang-format` symlinks to `.github/linters/.clang-format`.

---

## Tests (three tiers, increasing cost)

**Unit (`tests/unit/`, no NIC, no root)** — gtest against internal functions, compiled with `-Denable_unit_tests=true` which exposes internals. The only tier that runs anywhere.

```bash
./build.sh unit                                            # build + run all
./build_unit/tests/unit/UnitTest --gtest_filter='St40*'    # single test/suite after a build
```

**Integration (`tests/integration_tests/`, real VFs, root)** — `KahawaiTest`, one binary, one global `mtl_init()` for the whole run.

```bash
sudo ./build/tests/KahawaiTest --p_port 0000:c9:01.0 --r_port 0000:c9:01.1 \
  --auto_start_stop --gtest_filter='St20p*'
sudo ./build/tests/KahawaiTest ... --pacing_way tsc --gtest_filter='St20p*'   # software pacing fallback
sudo -E .github/scripts/gtest.sh                 # full CI orchestration (port discovery, sharding, retries)
```

`--level all` runs the non-mandatory cases; CI runs mandatory only. **`NoCtxTest.*` cases each need their own process** (DPDK EAL cannot re-init) — use `tests/integration_tests/noctx/run.sh`, never a filter matching several of them.

**Acceptance (`tests/acceptance/`, pytest, root, SSH-to-localhost)** — E2E through RxTxApp / FFmpeg / GStreamer; does not call the C API.

```bash
cd tests/acceptance
sudo -E ./venv/bin/python3 -m pytest \
  --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml \
  -m smoke -v
```

**Always the venv python** (`sudo python3` lacks `pytest_mfd_config`), **always both config flags**. Host prep: `.github/scripts/acceptance_setup.sh` (interactive, or `--auto`). Most cases need `/mnt/media` mounted over NFS. **Never edit `conftest.py`, `common/`, or `mtl_engine/` to make a test pass** — fix the environment or config.

**Fuzz (`tests/fuzz/`)** — single-packet libFuzzer harnesses over RX parsers; see `tests/doc/fuzz/index.rst`. Locally: `task ci:fuzz -- build && task ci:fuzz -- run`. PRs touching `lib/`, `include/`, or `tests/fuzz/` trigger the fuzz job of `fuzz_tests.yml`; main runs nightly.

---

## Runtime setup (needed for anything beyond unit tests)

```bash
sudo ./script/nicctl.sh create_vf 0000:af:00.0   # create + bind VFs to DPDK PMD
sudo sysctl -w vm.nr_hugepages=2048              # lost on reboot
sudo MtlManager                                  # lcore/queue arbitration daemon
```

`script/build_drivers.sh` builds the patched ICE module required for hardware rate-limit pacing (`--driver ice` for ICE only). A SEGFAULT in `iavf_tm_node_add` means the stock ICE driver is loaded.

---

## Architecture essentials

**Two-world rule.** Data plane (hugepage memory, `rte_spinlock_t`, lock-free rings, zero-copy, polling tasklets) must never call control-plane code: no `malloc`, no `pthread_mutex`, no `sleep`, no INFO-level logging. Control plane (session create/destroy, config, stats) may block. Violating this is the single most common way to break pacing.

**Cooperative tasklets, not threads.** A scheduler thread calls tasklet functions in a tight loop; a tasklet returns positive for "had work", 0 for "idle", and must never block — one blocked tasklet starves every other tasklet on that scheduler. Lcore mode (CPU-pinned) is default; `MTL_FLAG_TASKLET_THREAD` switches to pthreads for containers. Sessions are assigned to schedulers by weight in 1080p-equivalents; `MT_MAX_SCH_NUM = 18`.

**Three API layers.** Pipeline (`st20p_tx_get_frame`/`put_frame` — most apps; handles conversion and frame lifecycle) wraps Session (`st20_tx_create` + callbacks — direct frame/slice/RTP control) wraps Transport (internal: packet build, pacing, NIC).

**Layout.**

- `include/` — the entire public API (`mtl_api.h`, `st20_api.h`, `st_pipeline_api.h`, …). **Internal headers never go here.**
- `lib/src/mt_*.c` — core, non-media: scheduler, config, PTP, DMA, flow rules, mcast/IGMP, stats, RTCP, instance/manager IPC.
- `lib/src/dev/` — device layer: DPDK PMD (`mt_dev.c`) and AF_XDP (`mt_af_xdp.c`).
- `lib/src/datapath/` — the virtual data-path backend abstraction: queues, shared queues, shared RSS, kernel-socket path. This is what lets one packet TX/RX interface serve DPDK PMD, kernel socket, and AF_XDP.
- `lib/src/st2110/` — per-media TX/RX sessions plus builder/transmitter split for video, SIMD color conversion (`st_avx2.c`, `st_avx512*.c`), RX timing parser.
- `lib/windows/` — Windows shims.

**Video vs everything else.** Video is ~4500 packets/frame with µs pacing, slot-based reassembly, bitmap dedup, DMA offload, and a separate builder tasklet feeding a transmitter tasklet through an `rte_ring`. Audio/ancillary/fast-metadata are 1–8 packets/frame, per-frame pacing, single tasklet. Adding a new ST2110-xx type means copying the video session pattern and simplifying it — don't invent a new shape.

**Prefixes (enforced):** `mt_` core internals, `mtl_` public core API, `st_`/`st20_`/`st22_`/`st30_`/`st40_` media session APIs, `st20p_`/`st22p_`/`st30p_`/`st40p_` pipeline APIs, `tv_`/`rv_` TX/RX video internals, `tx_audio_session_`/`rx_audio_session_` etc. for the simpler media types.

---

## Repo-specific conventions (easy to trip on)

- `lib/` uses C (compiler default standard) + C11 `<stdatomic.h>` atomics; `tests/` uses C++.
- Log via `dbg()` / `info()` / `warn()` / `err()`, never `printf`.
- DPDK allocations via `mt_rte_zmalloc()` with an explicit `socket_id` from `mt_socket_id(impl, port)`; **NUMA mismatch roughly doubles DMA latency**.
- **Lock order: manager mutex → session spinlock**, never the reverse.
- **TX rings use `rte_ring_sp_enqueue_bulk()` (all-or-nothing)**; a partial `_burst()` enqueue breaks RTP sequence and pacing integrity.
- **Short `rte_eth_tx_burst()` → stash the remainder in `inflight[]` and return**; never discard, never retry in a loop.
- **RX frame buffers must be zero-initialized** — gaps from lost packets must read as zeros.
- **Mempool names need the `recovery_idx` suffix** to stay unique across recovery cycles.
