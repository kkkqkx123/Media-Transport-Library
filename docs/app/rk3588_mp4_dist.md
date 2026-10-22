# rk3588_mp4_dist — MP4 文件分发应用

`app/rk3588_mp4_dist/`：将 MP4 文件（H.264 + AAC）通过 **ST2110-22（视频）/
ST2110-30（音频）** 实时分发；接收端 MPP 硬解码 + DRM 显示 + ALSA 播放。
支持单板回环与 PC（TX）/ RK3588（RX）双机部署。详细运行说明见该目录内的
`README.md`。

## 数据链路

```
TX (mp4_dist_tx)                    网络                        RX (mp4_dist_rx)
────────────────                    ────                        ────────────────
FFmpeg 解复用 (tx/mp4_demux)        ST2110-22 视频              ST22 RX → bridge (rx/mtl_rx_bridge)
  → extradata 拼接 Annex-B          ST2110-30 音频 PCM          → playout buffer (rx/playout_buffer)
    (tx/extradata_handler)                                      → MPP 解码 (shared/mpp_decoder)
  → 播放列表调度 (tx/mp4_playlist)                               → 拼板合成 (rx/rx_compositor)
  → 发送调度 (tx/tx_scheduler)                                  → DRM 显示 (shared/drm_renderer)
  → ST22/ST30 TX (tx/mtl_tx_video, tx/mtl_tx_audio)             ST30 RX → A/V 同步 (rx/av_sync) → ALSA (rx/alsa_audio)
```

## 目录结构

| 目录 | 职责 |
|------|------|
| `tx/` | MP4 解复用、extradata 处理、播放列表、发送调度器、ST22/ST30 会话封装 |
| `rx/` | RX bridge、playout 缓冲、多窗口拼板合成（`rx_compositor`）、AV 同步、媒体时钟（`media_clk`）、H.264 预处理、ALSA 播放 |
| `shared/` | 两个进程共用的基础模块：MPP 解码器、DRM 渲染/模式、display_layout（窗口布局）、ring buffer、slab pool、timer、PTP 时间、端到端时间戳（`e2e_stamp`）与统计（`rx_e2e_stats`）、AU trace、TAI 时间戳（`tx_tai_ts`） |
| `config/` | JSON 配置（单板/双机）与 PTP 双机配置（`ptp/ptp4l-master/slave-unicast.conf`）、`scripts/start_ptp_dual_host.sh` |
| `tests/` | C 单测（display layout、DRM plane、时间戳、播放调度等）+ Python 日志校验（`check_pts_idx.py`、`check_playout_log.py`、`check_drm_log.py`、`compare_au_trace.py`）+ 单板/双板验证脚本（`run_single_board_plan_a.sh`、`mp4_dist_both.sh`、`verify_phase.sh`） |
| `mp4_dist_config.c` | JSON 配置加载（端口、IP、分辨率、码率、窗口布局、播放控制等） |

## 构建

```bash
cd app/rk3588_mp4_dist
meson setup build && ninja -C build
```

meson 选项（`meson_options.txt`）：

- `build_tx`（默认开）：`mp4_dist_tx`，只需 FFmpeg + MTL，**x86/ARM 均可编译**
- `build_rx`（默认开）：`mp4_dist_rx`，需 RK3588 + MPP + DRM + ALSA

在 PC 上只为 TX 单独构建时用 `-Dbuild_rx=false`。

## 部署形态

- **单板回环**：RK3588 上先启 RX、延时后启 TX（`tests/mp4_dist_both.sh`）。
- **双机**：PC 发流（`config/tx_dual_host.json`）、RK 收流
  （`config/rx_dual_host.json`），两机间需 PTP 时间同步
  （`start_ptp_dual_host.sh` + `config/ptp/` 单播配置）。

## 与 rk3588_end2end 的关系

面向"文件分发 + 多路播放"场景，链路更长（解复用、playout、AV 同步、拼板），
延迟目标比 end2end 的直播链路宽松；基础模块（MPP 解码、DRM 渲染、ring buffer、
slab pool 等）与 end2end 是各自的独立拷贝，修改时注意两处同步。
