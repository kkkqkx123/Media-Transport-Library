# rk3588_live_tx — RK3588 编码发送端

HDMI 实时采集→编码→MTL 发送的编码侧链路：V4L2 采集 → RGA 色彩转换 → MPP 硬件编码（H.264）→ MTL TX（ST 2110-22）。本包只产流，不收流；回环验证与解码呈现由 `rk3588_playout_rx` 进程经网络互通完成，不在本包内自环。

依赖 Rockchip MPP、librga、libdrm、V4L2、MTL；独立 meson 工程，**不接入 `app/meson.build`**，需在其目录内单独构建（交叉编译服务器）：

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
| `mtl_tx.c` | MTL 发送会话层封装 |
| `live_ingest_app.c` / `live_ingest_config.c` | 发送主程序与 JSON 配置 |
| `tests/` | 按层划分的单元测试（`test_hal` / `test_codec` / `test_pipeline` / capture 自测 `test_io`） |

## 回环验证（跨包）

本包只运行发送端，另起 `rk3588_playout_rx` 的接收进程收流显示。两侧帧率与分辨率显式对齐后方可互通。
