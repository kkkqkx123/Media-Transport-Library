# rk3588_live_tx — RK3588 编码发送端

编码侧唯一发送家：HDMI 实时采集链与文件帧源插件统一喂给 MTL 发送会话（ST 2110-22 视频 + ST 2110-30 音频）。本包只产流，不收流；回环验证与解码呈现由 `rk3588_playout_rx` 进程经网络互通完成，不在本包内自环。

默认源为 HDMI（V4L2 采集 → RGA 色彩转换 → MPP 硬件编码 → MTL TX）；`--source file --mp4 FILE` 选择文件帧源（MP4 解复用 → extradata 拼接 → SEI 打戳 → 同一发送宿接口），音频经 AAC 解码后由 ST30 会话发送。

依赖 Rockchip MPP、librga、libdrm、V4L2、MTL、FFmpeg（文件源）；独立 meson 工程，**不接入 `app/meson.build`**，需在其目录内单独构建（交叉编译服务器）：

```bash
meson setup build && ninja -C build
```

## 结构

| 目录/文件 | 说明 |
|---|---|
| `hal/` | 硬件抽象层：DMA 分配器（`dma_allocator` / `dma32_allocator`） |
| `io/` | 输入：`v4l2_capturer.c` HDMI/V4L2 采集 |
| `codec/` | 编码封装：`mpp_encoder`（MPP）、`rga_converter`（RGA 格式转换） |
| `pipeline/` | 管道层：`tx_pipeline`（采集→编码编排）；环形缓冲、SPS/VUI 与 SEI 时间戳复用 `rk3588_common` |
| `file/` | 文件帧源插件（阶段2）：`file_source`（解复用 + 播放列表 + 发送调度），喂给已有发送宿接口 |
| `mtl_tx.c` | MTL 视频发送会话层封装（支持 TAI/USER_PACING） |
| `mtl_tx_audio.c` | MTL 音频发送会话层封装（文件源 ST30，视频无音频） |
| `live_ingest_app.c` / `live_ingest_config.c` | 发送主程序与 JSON 配置 |
| `tests/` | 按层划分的单元测试（`test_hal` / `test_codec` / `test_pipeline` / capture 自测 `test_io`） |

## 回环验证（跨包）

本包只运行发送端，另起 `rk3588_playout_rx` 的接收进程收流显示。两侧帧率与分辨率显式对齐后方可互通。文件源先经首文件探针（宽高/帧率）对齐会话参数，播放列表切文件时重建 TAI 锚点。
