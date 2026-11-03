# rk3588_live_ingest — RK3588 低时延实时采集管道

HDMI 实时采集→编码→MTL 传输→MTL 接收→解码→DRM 渲染的端到端低时延演示管道。数据路径：V4L2 采集 → RGA 色彩转换 → MPP 硬件编码（H.264）→ MTL TX（ST 2110-20/22）→ MTL RX → MPP 解码 → DRM 屏幕输出。依赖 Rockchip MPP、librga、libdrm、V4L2、ALSA、MTL；独立 meson 工程，**不接入 `app/meson.build`**，需在其目录内单独构建：

```bash
meson setup build && ninja -C build
```

## 结构

| 目录/文件 | 说明 |
|---|---|
| `hal/` | 硬件抽象层：DMA 分配器（`dma_allocator` / `dma32_allocator`）、slab 池与定时器（复用 `rk3588_common`）、SEI 时间戳注入 |
| `io/` | 输入输出：`v4l2_capturer.c` HDMI/V4L2 采集，`drm_renderer.c` DRM 显示 |
| `codec/` | 编解码封装：`mpp_encoder` / `mpp_decoder`（MPP）、`rga_converter`（RGA 格式转换）、`sps_vui`（SPS/VUI 解析） |
| `pipeline/` | 管道层：`tx_pipeline` / `rx_pipeline`、`slice_reassembler`（分片重组）、环形缓冲（复用 `rk3588_common`） |
| `mtl_tx.c` / `mtl_rx.c` / `mtl_rx_bridge.c` | MTL 会话层封装（TX/RX 桥接） |
| `live_ingest_app.c` / `live_ingest_config.c` | 主程序与 JSON 配置 |
| `tests/` | 按层划分的单元测试（`test_hal` / `test_io` / `test_codec` / `test_pipeline` / `test_sps_vui`），各阶段可独立构建运行 |

## 构建（meson 自动按依赖探测分阶段）

- Phase 1（HAL）仅依赖 libdrm，任何环境可编；
- Phase 3（codec）/ Phase 4（pipeline）需要 MPP + librga；
- Phase 5（`live_ingest` 主程序）另需 MTL + json-c。缺依赖时对应目标自动跳过。
