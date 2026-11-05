# rk3588_live_tx — RK3588 编码发送端 (TX-only)

`app/rk3588_live_tx/`：RK3588 上的 ST 2110 编码发送端。**只产流，不收流**；采集、编码与发送全部在本包完成，回环验证与解码呈现由 `rk3588_playout_rx` 进程经网络互通完成（不在本包内自环）。

单二进制 `live_ingest`，可选 `--config <json>` 覆盖默认参数。支持两种帧源：

- 默认 HDMI：V4L2 采集 → RGA 色彩转换 → MPP 硬件编码 → MTL TX (ST2110-22)。
- 文件帧源：`--source file --mp4 FILE`，MP4 解复用 → extradata 拼接 Annex-B → SEI 打戳 → 同一发送宿接口；音频经 AAC 解码后由 ST30 会话发送。

## 数据链路

```
HDMI 帧源                                文件帧源
──────────────                          ──────────────
V4L2 采集 (io/v4l2_capturer)            FFmpeg 解复用 (file/file_source)
  → RGA 色彩转换 (codec/rga_converter)     → extradata 拼接 Annex-B (rk3588_common/extradata_handler)
  → MPP 编码 (codec/mpp_encoder)           → SEI 时间戳 (rk3588_common/sei_timestamp)
  → MTL TX (mtl_tx, ST2110-22)            → MTL TX (mtl_tx, ST2110-22)
                                              + MTL TX Audio (mtl_tx_audio, ST2110-30)
```

## 目录结构

| 目录/文件 | 职责 |
|-----------|------|
| `hal/` | 硬件/系统层：`dma_allocator` / `dma32_allocator`（DMA-BUF 分配）；环形缓冲、slab 池、定时器、SEI/TAI 时间戳复用 `rk3588_common` |
| `io/` | 输入：`v4l2_capturer`（V4L2 采集，DMA-BUF 导出） |
| `codec/` | 编码侧：`mpp_encoder`（Rockchip MPP 封装）、`rga_converter`（RGA 色彩/格式转换）；不含解码器 |
| `pipeline/` | `tx_pipeline`（采集→编码编排，直写 MTL 发送宿接口） |
| `file/` | 文件帧源插件（阶段 2）：`file_source`（解复用 + 播放列表 + 发送调度），喂给已有发送宿接口 |
| `mtl_tx.c` / `mtl_tx.h` | MTL 视频发送会话层封装（支持 TAI / 用户 pacing） |
| `mtl_tx_audio.c` / `mtl_tx_audio.h` | MTL 音频发送会话层封装（文件帧源 ST30；HDMI 帧源无音频） |
| `live_ingest_app.c` | 入口：模式/帧源解析、MTL 与 TX 栈生命周期 |
| `live_ingest_config.c` / `live_ingest_config.h` | JSON 配置加载（分辨率、帧率、UDP 端口、slice 数、码率、GOP、intra refresh、CPU 绑核、DRM 模式、日志级别等） |
| `tests/` | 按层单测：`test_hal` / `test_codec` / `test_io` / `test_pipeline` |

## 构建

```bash
cd app/rk3588_live_tx
meson setup build && ninja -C build
```

RK3588 交叉编译专用，无桌面/x86 回退。所有依赖均为必需：`libdrm`、`rockchip_mpp`、`librga`、`mtl`、`json-c`、FFmpeg（`libavformat/avcodec/avutil`，文件帧源）、`threads`。构建产物 `build/live_ingest`。

## 运行

```bash
./build/live_ingest [sec] [--config FILE] [--port P] [--lip IP] [--dip IP] [--udp PORT] [--audio-udp PORT] [--log-level L]
./build/live_ingest [sec] --source file --mp4 FILE [--loop/--no-loop] [--audio-udp PORT] [--use-tai]
```

环境准备：RK3588 无 DPDK poll-mode NIC，传输后端统一使用 **`native_af_xdp`**（软件 pacing，无 NIC 硬件限速），需先启动 `MtlManager` 并配置 hugepages（同仓库 README / `docs/env/RK3588_BOARD_ENV.md`），**无需** `script/nicctl.sh` 的 VF 绑定流程。`mtl_pmd_from_port()` 按端口名前缀（`native_af_xdp:` / `kernel:` / `dpdk_af_xdp:` 等）选择 PMD。

## 与 rk3588_playout_rx 的关系

本包只负责发送；接收、解码与显示由 `rk3588_playout_rx` 进程完成。两侧帧率与分辨率必须显式对齐方可互通。文件帧源在创建会话前先对首文件做探针（宽高/帧率）对齐会话参数，播放列表切文件时重建 TAI 锚点。

`dvs_media` 的 input 角色通过 in-tree 编译复用本包的捕获/编码/发送数据面（`hal/`、`io/v4l2_capturer`、`codec/`、`pipeline/tx_pipeline`、`mtl_tx`），应用编排 `live_ingest_app.c` 不引入。