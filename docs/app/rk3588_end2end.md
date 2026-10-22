# rk3588_end2end — RK3588 端到端低延迟直播链路

`app/rk3588_end2end/`：在 RK3588 上跑通 **采集 → 编码 → MTL 传输 → 解码 → 渲染**
的完整低延迟链路。单二进制 `e2e_app` 支持 `tx` / `rx` / `both`（单板回环）三种模式，
可选 `--config <json>` 覆盖默认参数。

## 数据链路

```
TX 模式                                RX 模式
──────────────                         ──────────────
V4L2 采集 (io/v4l2_capturer)           ST2110 RX (mtl_rx)
  → RGA 色彩转换 (codec/rga_converter)   → slice 重组 (pipeline/slice_reassembler)
  → MPP 编码 (codec/mpp_encoder)         → MPP 解码 (codec/mpp_decoder)
  → MTL TX (mtl_tx)                      → DRM 渲染 (io/drm_renderer)
```

`both` 模式在同一进程内同时创建 TX/RX 两个栈（单板回环验证）。

## 目录结构

| 目录 | 职责 |
|------|------|
| `codec/` | 编解码插件层，统一接口 `codec_plugin.h`；`mpp_encoder.c` / `mpp_decoder.c` 封装 Rockchip MPP，`rga_converter.c` 做 RGA 色彩/格式转换 |
| `hal/` | 硬件/系统层：`dma_allocator`（DMA-BUF 分配）、`slab_pool`（帧池）、`timer`（单调时钟）、`sei_timestamp`（SEI 时间戳打点） |
| `io/` | 输入输出：`v4l2_capturer`（V4L2 采集，DMA-BUF 导出）、`drm_renderer`（DRM/KMS 显示） |
| `pipeline/` | 流水线：`tx_pipeline` / `rx_pipeline`（线程编排）、`ring_buffer`（帧队列）、`slice_reassembler`（码流切片重组） |
| `mtl_tx.c` / `mtl_rx.c` / `mtl_rx_bridge.c` | MTL 会话层封装：初始化参数、TX/RX 会话创建、桥接回调 |
| `e2e_app.c` | 入口：模式解析、`e2e_cfg` 配置、TX/RX 栈生命周期 |
| `e2e_config.c` | JSON 配置加载（分辨率、帧率、UDP 端口、slice 数、码率、GOP、插帧刷新、CPU 绑核、DRM 模式、日志级别等） |
| `tests/` | codec / hal / io / pipeline 四组单测 |
| `ref/` | MPP 参考实现（`mpi_dec_test` 及工具函数），调试编解码问题时对照用 |

## 构建

```bash
cd app/rk3588_end2end
meson setup build && ninja -C build
```

依赖按阶段可选：Phase 1（hal/io）只需 libdrm；`codec/` 需要 `rockchip_mpp` + `librga`
（缺失时自动跳过，仅编译无编解码能力的栈）。`mtl`、`json-c` 为可选依赖。

## 运行

```bash
./build/e2e_app both                 # 单板回环
./build/e2e_app tx --config e2e.json # 仅发送
./build/e2e_app rx                   # 仅接收
```

先准备环境：hugepages、网卡 VF 绑定 DPDK（`script/nicctl.sh`），见仓库主文档
"Runtime setup"。ARM 平台使用软件 pacing（TSC）。
