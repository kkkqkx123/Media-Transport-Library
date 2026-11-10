# 手动调试脚本清单与对照说明

> 清单范围：仓库中需上板手跑的三组脚本。同步路径规范见
> `docs/dev/board-mtl-sync.md`。设计口径见 `docs/tests/` 下各 design 文档，
> 本清单只做“脚本—板端位置—输入输出—对照设计”的索引，不重复设计内容。

## 1. 单帧探针（`debug/` → 板 `src/debug/`，产物进 `bin/`）

| 本地 | 板端源码 | 板端产物 | 输入 → 输出 | 对照设计 |
|---|---|---|---|---|
| `debug/v4l2_cap_latency.c` | `src/debug/v4l2_cap_latency.c` | `bin/v4l2_cap_latency` | `/dev/video0` → 帧间隔分布 + `--csv` | `rk3588-capture-encode-decode-perf-design.md` 采集基线 |
| `debug/mpp_enc_frame_latency.c` | `src/debug/mpp_enc_frame_latency.c` | `bin/mpp_enc_latency` | `streams/*.nv12/bgr3` → 单帧提交到产出耗时 + `--csv`/`--out h264` | 同上编码上限；BGR 直通语义见 `codec-latency-semantics.md` |
| `debug/mpp_dec_frame_latency.c` | `src/debug/mpp_dec_frame_latency.c` | `bin/mpp_dec_latency` | `streams/*.h264` → 送入到首帧耗时 + `--csv` | 同上解码上限；`--wait-ms` 为单 AU 拉取截止期，必须大于最慢真实帧，否则慢帧被误记 `no_output`（旧板二进制无此参数，已重编收敛） |
| `debug/rga_blit_latency.c` | `src/debug/rga_blit_latency.c` | `bin/rga_blit_latency` | 堆内缓冲自拷贝/缩放 → 单次 blit 耗时 + `--csv` | 同上转换开销；`--srcfmt bgr3` 覆盖生产采集路径 |
| `debug/bgr_stride_matrix.sh` | `src/debug/bgr_stride_matrix.sh` | 板上直跑 | 生成条纹 BGR/NV12 → 4 格编码对照 verdict | BGR 对齐取证（CLEAN/640 周期 TILED） |
| `debug/v4l2_fmt_matrix.sh` | `src/debug/v4l2_fmt_matrix.sh` | 板上直跑 | 逐 fourcc S_FMT+G_FMT 回读 | 协商矩阵，以本矩阵为准，不信枚举列表 |
| `debug/v4l2_stream_check.sh` | `src/debug/v4l2_stream_check.sh` | 板上直跑 | 输入状态→时序快照→试采→dmesg | 退出码定责：0 可采、1 格式拒、2 无信号、3 设备忙 |

## 2. 显示/音频/网络（`debug/tests/` → 板 `src/debug/tests/`）

| 本地 | 板端 | 对照设计 |
|---|---|---|
| `debug/tests/drm_smoke.c` → `bin/drm_smoke` | 点屏冒烟，退出恢复原 CRTC | `rk3588-drm-verification-design.md`、`docs/dev/tests/rk3588-drm-smoke.md` |
| `debug/tests/hdmi_passthrough.c` → `bin/hdmi_passthrough` | V4L2 MMAP 采集→CPU 转 NV12→DRM 全屏，不经编解码/网络 | A/B 对照的 A 端（直通正常而链路异常才查编解码/网络） |
| `debug/tests/display_compare.sh`（可执行副本进 `bin/`） | 编排 A（直通）→ B（完整链路 `run_tx_rx_e2e.sh`），看屏填结论 | 同上 A/B 方法 |
| `debug/tests/alsa_tone.c` → `bin/alsa_tone` | 指定 PCM 设备播 1s 正弦，PASS/FAIL 即结论 | 音频设备可用性冒烟 |
| `debug/tests/netem_inject.sh` | `tc netem` 丢包/时延/抖动/乱序注入与清除 | 弱网对照（`clear` 恢复） |
| `debug/tests/board_env.sh`（可执行副本进 `bin/`） | `status` 只读门禁；`prepare/run/release/restore` 释放 DRM/声卡/video0/UDP 并恢复桌面 | 各 e2e 脚本的统一环境门禁（`run_tx_rx_e2e.sh` 等 preflight 均调它） |

## 3. 性能编排（`tests/perf/` → 板 `src/perf/`，输出进 `perf/`）

| 本地 | 板端 | 输入 → 输出 | 对照设计 |
|---|---|---|---|
| `tests/perf/gen_test_stream.sh` | `src/perf/gen_test_stream.sh` | 无 HDMI 时生成 `streams/` 预置流；`--4k` 追加 4K 时钟流 | `rk3588-capture-encode-decode-perf-design.md` 回归输入 |
| `tests/perf/common/env_probe.py` | `src/perf/common/env_probe.py` | 板型/内核/MPP/RGA/V4L2 → `perf/env.json`，每轮必跑 | 同上环境基线 |
| `tests/perf/common/hw_load_sample.py` | 同级映射 | `rkvenc/rkvdec/rkrga/CPU` 采样 | 争用排除（跑画像前停业务或记录基线） |
| `tests/perf/tx_capture_encode_perf.py` | 同级映射 | 需 `live_ingest` + HDMI 真实源 → `perf/tx*/tx_capture_encode.{json,md}` | 发送侧画像与预算 |
| `tests/perf/rx_decode_perf.py` | 同级映射 | `--mode offline` 吃预置 h264（优先 `bin/mpp_dec_latency` 探针）→ `perf/rx*/rx_decode.{json,md}` | 接收侧画像与预算 |
| `tests/perf/e2e_latency_budget.py` | 同级映射 | 单板回环 → `perf/e2e*/e2e_latency.{json,md}` | 端到端预算表 |
| `tests/perf/common/{report.py,thresholds.json}` | 同级映射 | JSON → Markdown 报告；阈值初值，三轮收敛后调 | 判定阈值来源 |

## 4. 端到端验证（`app/rk3588_playout_rx/tests/` 手动子集 → 板 `src/playout-tests/`，待跑项拷入 `bin/`）

| 脚本 | 板端 | 对照设计 |
|---|---|---|
| `run_tx_rx_e2e.sh` | `src/playout-tests/` + `bin/` 可执行副本，单板回环默认 60s | `rk3588-tx-rx-e2e-verification-design.md` L0–L7；报告见 `rk3588-tx-rx-e2e-validation-report.md` |
| `run_multinode_composite.sh` | 同上；`OUT_DIR` 先定义后使用（此前引用未定义变量，`set -u` 下直接失败，已修复） | `rk3588-multinode-composite-verification-design.md` T0–T3；报告见 `rk3588-multinode-composite-validation-report.md`；音频正式结论见 §5 |
| `manual_check_visual.sh` | 同上 | 目检流程（与 `display_compare.sh` 互补） |
| `run_dualboard_interop.sh`、`run_dualoutput_sameframe.sh` | 同上，未来 F1–F3，本阶段只备不用 | 见两 design 文档的未来任务节 |
| `verify_phase.sh`、`plan_a_verify.sh`、`run_single_board_plan_a.sh` | 同上 | 单路 Plan A 阶段性脚本，`verify_phase.sh` 用 `FILE_PLAYOUT_DUMP_RX` 取流后二次验证 |
| `check_playout_log.py`、`check_drm_log.py`、`check_pts_idx.py`、`compare_au_trace.py`、`verify_vrx_margin.py` | 同上离线跑 | 日志/AU 轨迹/PTS/VRX 余量检查器，输入为日志或抓包，见各脚本头注释 |
| `rx_minimal.c` | 同上源码备用 | 最小 RX 复现（非 meson 单元测试） |

`test_display_layout.c` 等 meson 单元测试与 `meson.build` 不同步上板；
`tests/` 板旧目录（仅 2 个 py 的手抄）已删除，由 `src/playout-tests/` 替代。

## 5. 音频正式验证结论（2026-11-10 板上实测，`ssh board`）

前置：`live_ingest` + `file_playout_rx` 已按 `docs/dev/board-mtl-sync.md` §3 重推至 `bin/`；`run_multinode_composite.sh` 同步至 `src/playout-tests/` 并拷入 `bin/`；`md5sum` 双端一致。声卡就绪（播 `hdmi0/hdmi1/dp/es8388`，采 `hdmiin/es8388`），片源 `/usr/local/test.mp4` 为 `1080p24 + AAC 48k 立体声 FLTP`。

| 用例 | 结果 | 证据 |
|---|---|---|
| T1 文件源单发单收 30s（含音频断言） | 通过 | 视频 `730 frames/drops=0`、`dec/mtl_rx 480/480` 零丢零坏；音频 TX `audio exit 30023 frames`、RX `mtl_rx_audio 26927 frames, 0 drops`；TX 日志 `audio resample 48k/2ch/fltp到48k/2ch/s16`，RX `audio=on` + `audio anchor` |
| 重采样分支 44100Hz 单声道自造片源 15s | 通过（音频项全过） | TX `audio resample 44100Hz/1ch/fltp到48000Hz/2ch/s16`，`audio exit 15045 frames`；RX `12010 frames, 0 drops`；唯一 FAIL 为稳态窗口数（15s 仅 2 窗口，属时长不足，非音频故障） |
| 反例 TX 直播坏声卡 | 按预期失败 | `--audio-source alsa --audio-device nonexistent` 报 `ALSA capture unavailable` 后 `exit=1`，不再静默 `video-only` |
| 反例 RX `audio_udp=0` 纯视频 | 通过 | `audio=off`，无 `alsa: ready`/`mtl_rx_audio` 建连行，视频照常 |
| T0 HDMI 基线 30s | 通过（重跑确认） | 初次失败为超时杀脚本残留进程争用端口所致（`build ret -283`=应用无帧可发、`trs -142`=发送环无包，均为结果而非原因）；清理残留后重跑 `T0 30` 全过：收流 `1616 pkt/1615 dec`、发送 `1795 cap/1793 enc`、零丢零坏 |
| T3 独立运行 | 脚本前置缺失 | 依赖 T2 生成的 `multinode_2lane.json`，单独跑 T3 即 `FileNotFoundError`（历史脚本排序问题，与音频改动无关） |
