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

## 目录结构

| 目录/文件 | 职责 |
|-----------|------|
| `rx/` | RX 数据面：`mtl_rx_mp4` / `mtl_rx_bridge`（MTL 收包与桥接）、`playout_buffer`（播放缓冲）、`rx_pipeline`（接收管线编排）、`rx_compositor`（多窗口拼板合成）、`av_sync`（A/V 同步）、`media_clk`（媒体时钟）、`h264_prep`（H.264 预处理）、`alsa_audio`（ALSA 播放）、`mtl_rx_audio`（ST30 接收） |
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