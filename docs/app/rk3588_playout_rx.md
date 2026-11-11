# rk3588_playout_rx — RK3588 解码接收端 (RX-only)

`app/rk3588_playout_rx/`：RK3588 上的 ST 2110 解码接收端。**只收流，不产流**；MTL 接收 → MPP 硬解码 → 播放缓冲 → AV 同步 → DRM 多平面合成 → HDMI 显示，音频走 ST30 RX → ALSA 播放。发送端由 `rk3588_live_tx` 经网络供给（文件帧源已迁入编码侧，见 `docs/plan/rk3588/RK3588_ENCODE_DECODE_ROLE_REFACTOR.md` 阶段二）。

单二进制 `file_playout_rx`，可选 `--config <json>` 覆盖默认参数。参考流参数探针固定用仓库内 `InTheGrey.mp4`（1920×1080 @ 23.976fps，路径 `../InTheGrey.mp4`）做期望流描述；实际网流由编码端产生，`--fps/--width/--height` 必须与发送端一致。

## 数据链路

```
编码端 (rk3588_live_tx)              网络                        接收端 (file_playout_rx)
─────────────────────────              ───────────              ─────────────────────────
V4L2 → RGA → MPP编码 → MTL TX  ──►  ST2110-22 视频码流  ──►  ST22 RX → bridge (rx/mtl_rx_bridge)
                                                          → playout buffer (rx/playout_buffer)
                                                          → MPP 解码 (shared/mpp_decoder)
                                                          → 拼板合成 (rx/rx_compositor)
                                                          → DRM 显示 (shared/drm_renderer)
                                     ST2110-30 音频 PCM  ──►  ST30 RX → A/V 同步 (rx/av_sync) → ALSA (rx/alsa_audio)
```

## 解码会话模型：单流与多流

两条路径由 `file_playout_app.c` 按 `pip_stream_count` 选择：为 0 走单流
`run_legacy_pipeline`（`rx/rx_pipeline.c`），大于 0 走多流 `run_compositor`
（`rx/rx_compositor.c`）。`dvs_media` 只走 compositor（`engine/engine_ctx.c`
的 `build_compositor`），与单流路径无关。

| | 单流 `rx_pipeline` | 多流 `rx_compositor` |
|---|---|---|
| MTL 会话 | 1 个 ST22 RX 会话（`mtl_rx_mp4`，`--udp/--sip` 指定） | 每 lane 1 个 ST22 RX 会话（各自 `udp_port/sip/src_ip`），共享一个 `mtl_handle` |
| 解码会话 | 1 次 `mpp_decoder_create`（独立 MPP DEC ctx）+ 1 个 decode 线程 | 每 lane 1 次 `mpp_decoder_create` + 1 个 decode 线程（亲和 `cpu_decode+i`） |
| 桥接/缓冲 | 1 个 bridge + 1 个 playout_buffer | 每 lane 独立 bridge + rings/slab + playout_buffer；各 lane bridge 线程默认都钉在同一个 `cpu_bridge` 上，争用量化见下文 `BR[]` 行 |
| 显示 | render 线程直显（全屏 PageFlip 或单 viewport plane） | 单个 render 线程经 `drm_renderer_display_layers` 一次合成全部 lane；`av_sync`（`playout_delay`）全局共享 |
| 解码延时观测 | 有（`dec_p50/dec_p99` 滑动窗口） | 有（每 lane `dec_p50/dec_p99`，与单流同口径，见 `RX[]` 行尾） |

上限：`MP4_PIP_V1_NV12_LANES=4`（固定 Esmart plane 57/79/101/123）。超限配直
截断到 4 并告警（`file_playout_pip_validate`），第 5 路起不建 MTL 会话、
不解码、不显示。11-11 曾短暂放开到 8 并做运行时 plane 探针，板上实测同
CRTC 仅 4 个 NV12 plane（第 5 路 fail-fast 符合预期），证实 4 即硬件上限，
当日即回退到 4（探针代码已拆除，不留死代码）。同时点亮 5~8 路需 RGA 预合成
等后续手段，见归档 `docs/archive/tx-rx/2026-11-11-eight-lane-probe-revert.md`。
注意：`dvs_platform` 的单节点窗口预算同样为 4（`max_windows_per_node`），
两侧一致。

MPP 硬件层面各 lane 是独立 ctx，共享同一块 rkvdec 分时；并发 stretch 实测
（x4 sync aggregate 约 776~800fps）见
`docs/archive/perf/2026-11-11-codec-latency-truth.md` §5。

## 共用解码核心与打点

`rx/rx_decode_common.c` 抽出两条路径完全相同的部分：AU 送入
（`h264_prep` + 整 AU/分片 put）与拉取 drain 循环（含 `au_pts + n*周期` 的
pts 换算、idle 退避），以及 feed 到首帧的滑动窗口延时
（`dec_p50/dec_p99`，`RX_DEC_LAT_WIN` 300）。调用方只保留线程循环与帧落子
回调（单流：计数 + 延迟采样 + 入队直显；多流：计数 + 延迟采样 + 按 lane 入队）。
单流与多流的解码延时口径自此一致，均为“送入到首个可显示帧”。

`mtl_rx_bridge` 新增争用量化打点（`BR[]` 行）：`work_ema/work_p50/work_p99`
为 memcpy+SEI 解析+入队段耗时，`wait_ema` 为阻塞等待 MTL 包的时间，
`ring_max` 为观测到的 `mtl_ring` 最大堆积。判定方法：先跑单 lane 基线
（同内容），再加到 2/4/8 lane 对比——共享一核 `cpu_bridge` 出现争用时，
`work_ema` 相对基线膨胀、`wait_ema` 向零坍缩、`ring_max` 持续抬高三者同现；
三者任一单项变化都可能是内容或码率差异，不单独作为争用证据。 remediation
（bridge 亲和按 lane 散列，如 `cpu_bridge+i`）以前三项数据为门控，不预设。

## 设计评估与遗留清理

1. 双路径并存合理，legacy 不可删。compositor 要求 layout 为合成模式才有输出
   （`display_layers` 依赖 `compositor_active`），单流全屏布局走 compositor
   无输出；legacy 是目前唯一可用的单流全屏直显路径。删除 legacy 的前提是先修好
   单 lane 全屏合成路径，在此之前两条都保留。
2. 已清理：`app/dvs_media/meson.build` 曾把 `rx/rx_pipeline.c` 编进 `dvs_rx`，
   但 engine 只用 compositor、无任何符号引用，属 dead object，已移除该行，
   不影响行为；新共用核心 `rx/rx_decode_common.c` 已加入两边 `meson.build`。
3. 已收敛：decode/feed/drain/延时窗口的重复已抽入 `rx_decode_common`，
   render 线程与管线编排仍各自保留（直显与合成的落子语义不同，不强行合并）。
4. 待板上验证（本地禁编译，见下节）：8 lane 创建、5~8 lane plane 探针解析、
   `test_display_layout` 新增 6 lane 用例、`dec_p50/dec_p99` 与 `BR[]` 行输出；
   `dvs_platform` 单节点预算 4 保持不变。
5. `app/rk3588_playout_rx/` 下的 `TMP_RX_PLAYOUT_OPTIMIZATION_PLAN.md` 与
   `IMPLEMENTATION_PLAN.md` 为 06-16/17 临时规划稿，与当前实现已脱节，仅作历史
   存档阅读，不作为设计依据。

## 验证（11-11 已执行，devs 交叉编译 + 板上实测）

- 交叉编译：`rk3588_playout_rx`、`rk3588_live_tx`、`dvs_media` 全过
  （devs 上用 buildroot meson 1.3.1；`script/build_rk3588_cross.sh` 的
  `-Dbuild_tx/-Dbuild_rx` 已过期，且其改写的 cross 文件需用新 meson——
  脚本待修，本次为手动等价命令）。产物均为 aarch64，`libdvs_rx.a` 含
  `rx_decode_common` 且不再含 `rx_pipeline`。
- 单测：板上 gcc 直编 `test_display_layout`，0 failed。
- 板上回环：单 lane 出数正常，`dec_p50/dec_p99` 与 `BR[]` 行齐全
  （`put_fail=0`，`ring_max=0`，单 lane 无争用符合预期）。
- 板上回环：4 lane（4 路文件源 TX）350 composites 全 lane 出数，dec/BR
  四路齐全且 work 值一致，无争用迹象。8 lane 探针当日验证后即回退
  （同 CRTC 仅 4 个 NV12 plane，见归档），回退本身不再复测。

## 目录结构

| 目录/文件 | 职责 |
|-----------|------|
| `rx/` | RX 数据面：`mtl_rx_mp4` / `mtl_rx_bridge`（MTL 收包与桥接，含 `BR[]` 争用量化打点）、`playout_buffer`（播放缓冲）、`rx_pipeline`（单流接收管线编排）、`rx_compositor`（多窗口拼板合成，8 lane）、`rx_decode_common`（两条路径共用的解码送入/拉取核心与解码延时窗口）、`av_sync`（A/V 同步）、`media_clk`（媒体时钟）、`h264_prep`（H.264 预处理）、`alsa_audio`（ALSA 播放）、`mtl_rx_audio`（ST30 接收） |
| `shared/` | 跨模块共享：`mpp_decoder`（MPP 硬解码）、`drm_renderer`（DRM 渲染/模式）、`mp4_demux`（MP4 探针，参考流描述）、`extradata_handler`（附加数据拼接）、`rx_e2e_stats`（统计） |
| `config/` | JSON 配置（`rx_dual_host.json.example`）与 PTP 双机配置（`ptp/`） |
| `scripts/` | `start_ptp_dual_host.sh`（PTP 启停） |
| `file_playout_config.c` / `file_playout_config.h` | JSON 配置加载（端口、IP、分辨率、窗口布局、播放控制等）与 DRM 模式辅助 |
| `file_playout_rx_app.c` | 接收端入口 |
| `tests/` | C 单测（`test_media_clk`、`test_drm_mode`、`test_display_layout`）+ `rx_minimal` + Python 日志校验（`check_pts_idx.py`、`check_playout_log.py`、`check_drm_log.py`、`compare_au_trace.py`）+ 单板/双板验证脚本 |

本包不再包含 TX 发送侧；`mp4_demux` / `extradata_handler` 仅作为接收侧参考流描述与附加数据处理保留，已上移到 `shared/`。

## 构建

```bash
cd app/rk3588_playout_rx
meson setup build && ninja -C build
```

RK3588 交叉编译专用，无桌面/x86 回退。所有依赖均为必需：`mtl`、`libavformat/avcodec/avutil`（探针）、`json-c`、`rockchip_mpp`、`libdrm`、`alsa`、`threads`。构建产物 `build/file_playout_rx`。

## 部署形态

- **单板回环**：RK3588 上分别启动接收进程与发送进程（先起 RX、延时后起 `rk3588_live_tx`），`tests/run_single_board_plan_a.sh` / `tests/plan_a_verify.sh`。
- **双机**：`rk3588_live_tx` 发流（或 PC 发流）、RK 收流（`config/rx_dual_host.json.example`），两机间需 PTP 时间同步（`scripts/start_ptp_dual_host.sh` + `config/ptp/` 单播配置）。

传输后端：RK3588 无 DPDK poll-mode NIC，默认使用 **`native_af_xdp`**（软件 pacing），需 `MtlManager` + hugepages；PMD 也可按端口名前缀选择 `kernel:` / `dpdk_af_xdp:` 等。详细见该目录 `README.md`。

## 与 rk3588_live_tx 的关系

面向"接收/解码/显示"场景，链路为 ST2110 接收 → MPP 解码 → playout → AV 同步 → DRM/ALSA；基础模块（MPP 解码、DRM 渲染、ring buffer、slab pool 等）与 `rk3588_live_tx` 的基础模块分别维护，修改时注意两处同步。`rk3588_live_tx` 仅发送，`rk3588_playout_rx` 仅接收，两侧帧率与分辨率必须显式对齐。

`dvs_media` 的 output 角色通过 in-tree 编译直接复用本包的 `rx/` 与 `shared/`（含 `mp4_demux` 探针与 `extradata_handler`），编辑这些文件会影响 `dvs_media` 的构建与行为。