# dvs_media 与 rk3588 app 的集成分析

> 分析对象：`app/dvs_media`（DVS 节点媒体引擎，纯 C）
> 对照对象：`app/rk3588_file_playout`、`app/rk3588_live_ingest`、`app/rk3588_common`
> 结论先行：**dvs_media 由 `rk3588_file_playout` 的 RX 侧演进而来，通过 meson in-tree 编译直接复用其源码（非复制）；`rk3588_live_ingest` 的 TX 数据面已接入，input 角色经 V4L2 捕获→RGA→MPP 编码→MTL 发送出流。**

---

## 1. 集成方式：in-tree 编译复用，不复制源码

`app/dvs_media/meson.build` 是一个**独立的 meson 工程**（不是 `app/meson.build` 的子工程）。它不把 `rk3588_file_playout` / `rk3588_common` 的源文件复制到本地，而是直接用相对路径 `files('../rk3588_file_playout/...')` 把上游源码编进本工程的 `static_library`，再链接到 `dvs_media` 可执行文件。

这种"借用源码 + 本工程额外封装一层控制面"的模式，正是 AGENTS.md 中描述的"由 file_playout 演进而来"的工程落地。

三个静态库构成复用骨架：

| 静态库 | 来源 | 内容 |
|---|---|---|
| `dvs_shared` | `rk3588_common/src/*` + `rk3588_file_playout/shared/*` | 无锁环、slab 池、定时器、DRM 模式、布局、PTP、E2E 时间戳等基础组件 |
| `file_playout_probe` | `rk3588_file_playout/tx/mp4_demux.c` | FFmpeg 解封装（MP4 probe） |
| `dvs_rx` | `rk3588_file_playout/rx/*` + 部分 `shared/*` + `tx/extradata_handler.c` | 接收/解码/合成/播放缓冲/AV 同步/ALSA 出声的完整 RX 数据面 |
| `dvs_tx`（有 MPP+RGA 时链接） | `rk3588_common/src`时间戳相关 + `rk3588_live_ingest/hal`、`io/v4l2_capturer`、编码侧 `codec`、`pipeline/tx_pipeline`、`mtl_tx` | 捕获/RGA 转换/MPP 编码/MTL 发送的完整 TX 数据面，环/ slab /定时器经 `dvs_shared` 复用，不重复编译 |

最终 `dvs_media` 可执行文件链接 `dvs_shared + dvs_rx + file_playout_probe`，有 MPP+RGA 时再链接 `dvs_tx`，并额外编译本工程 `engine/` 与 `ipc/`。

---

## 2. 具体复用的源文件清单

来自 `app/dvs_media/meson.build:46-119` 的精确映射：

**`rk3588_common/src/`（基础无状态组件，第一批已纳入 `sps_vui`、`sei_timestamp`、`display_layout`、`drm_mode`、`ptp_time`）**
- `ring_buffer.c` `slab_pool.c` `timer.c` `sps_vui.c` `sei_timestamp.c` `display_layout.c` `drm_mode.c` `ptp_time.c`
- 可选依赖 MTL（找到时以 `-DHAVE_MTL=1` 编译，见 `rk3588_common/meson.build`）

**`rk3588_file_playout/shared/`（跨应用共享的 RX/解码/显示辅助）**
- `au_trace.c` `tx_tai_ts.c` `e2e_stamp.c` `rx_e2e_stats.c`
- `mpp_decoder.c`（MPP 解码封装）`drm_renderer.c`（DRM 渲染）

**`rk3588_file_playout/rx/`（RX 数据面核心）**
- `alsa_audio.c` `playout_buffer.c` `av_sync.c` `media_clk.c` `mtl_rx_mp4.c` `mtl_rx_bridge.c` `mtl_rx_audio.c` `h264_prep.c` `rx_pipeline.c` `rx_compositor.c`

**`rk3588_file_playout/tx/`（仅取与 RX 路径共用的部分）**
- `mp4_demux.c`（编入 `file_playout_probe`）`extradata_handler.c`（编入 `dvs_rx`）

**`rk3588_live_ingest/`（TX 数据面，已接入 `dvs_tx`）**
- `hal/dma_allocator.c` `hal/dma32_allocator.c`（DMA 分配）
- `io/v4l2_capturer.c`（V4L2 捕获）
- `codec/rga_converter.c` `codec/mpp_encoder.c`（RGA 转换 + MPP 编码，仅编码侧；解码器不引入，dvs_media 只用 file_playout 的解码实现）
- `pipeline/tx_pipeline.c`（捕获→编码编排 + MTL sink 钩子；重组器是 RX 路径的，不需要）
- `mtl_tx.c`（ST22 发送会话）
- `rk3588_common/src/sei_timestamp.c` `sps_vui.c`（时间戳与 SPS 注入，随 TX 链路编入）
- 应用编排 `live_ingest_app.c` 不引入，由 dvs_media 自身后端编排代替

**根级配置**
- `rk3588_file_playout/file_playout_config.c`（编入 `dvs_media` 主目标，提供双口 MTL 初始化与 DRM 模式辅助）

---

## 3. 复用的接口/函数层（dvs_media 调用了哪些 file_playout 符号）

dvs_media 自身只写"控制面 + IPC"，所有数据面动作都委托给 file_playout 的公开符号（`engine_ctx.c` 等是直接证据）：

| 类别 | 复用符号 |
|---|---|
| 配置/初始化 | `file_playout_config_load` `file_playout_mtl_init_dual` `file_playout_apply_drm_mode` `file_playout_mode_dims` `file_playout_pip_validate` |
| 合成器 | `rx_compositor_create` `rx_compositor_start` `rx_compositor_stop` `rx_compositor_free` `rx_compositor_try_fast_apply` `rx_compositor_report_stats` `rx_compositor_lane_*` |
| 数据结构 | `file_playout_cfg` `file_playout_pip_stream` `MP4_PIP_MAX_LANES`（`rk3588_common/include/rk3588_common/display_layout.h`，第一批已由 file_playout `shared/` 上移） |
| 编解码/渲染 | `mpp_decoder.c` `drm_renderer.c`（经 `rx_compositor` 间接调用） |
| MTL 公共 API | `mtl_version` `mtl_ptp_read_time` `mtl_stop` `mtl_uninit` |
| 编码/发送 | `mtl_tx_create/start/stop/free` `mtl_tx_acquire_fb/publish_fb/abort_fb` `tx_pipeline_create/start/stop/free`（经 `dvs_tx_backend_entry` 间接调用） |

注意 `engine_ctx.c` 直接 `#include "file_playout_config.h"` 与 `"display_layout.h"`（后者第一批已上移 `rk3588_common`），说明 dvs_media 把 donor 的头文件当作"半公开 API"使用——这是 in-tree 复用的代价：donor 的内部头对 dvs_media 完全可见，二者强耦合。

---

## 4. 与 rk3588_live_ingest 的关系：TX 数据面已接入

- `app/dvs_media/meson.build` 以 `dvs_tx` 静态库 in-tree 编译 live_ingest 的捕获/编码/发送链路（hal DMA、V4L2 捕获、RGA + MPP 编码、`tx_pipeline`、`mtl_tx`），解码器与应用编排不引入。
- `engine/tx_session.c` 仍是纯参数校验 + 状态机（无 MTL/MPP 依赖，x86 可单测）；`engine/tx_backend_live_ingest.c` 实现数据面入口，`engine_ctx.c` 在创建时安装、启动前下发 MTL 句柄与端口配置、各 teardown 路径统一停止。
- 无后端构建仍诚实降级：TX 只报 `DVS_TX_CONFIGURED` + `backend_pending`，不谎称出流。

因此：
- **RX 侧（output role）**：完全复用 file_playout 的 RX 数据面。
- **TX 侧（input role）**：复用 live_ingest 数据面，已闭环。

---

## 5. dvs_media 自己新增的"受控层"（file_playout 没有的部分）

演化方向是**受控运行**：由 `dvs_agent` 通过 IPC 驱动布局变更，而非 file_playout 的"命令行一次性跑完"。新增模块：

| 模块 | 职责 |
|---|---|
| `engine/engine_ctx.c` | 引擎上下文：双口 MTL 生命周期、合成器构建/重建、`role` 互斥（input 停 RX 并拒新布局、output 停 TX）、TX 编排与统计循环 |
| `engine/session_mgr.c` | 生命周期入口（对 IPC 层封装 `dvs_engine_*`） |
| `engine/session_admit.c` | 白名单 + LRU 准入（source 准入控制） |
| `engine/layout_executor.c` | 两阶段 `prepare`（暂存待定布局，不动运行画面）/ `commit`（换上待定布局）/ `abort` / `clear`；从 JSON 解析 `streams`/`layout` 到 `file_playout_pip_stream` |
| `engine/audio_arbiter.c` | ALSA 音频路由仲裁 |
| `engine/ipc_engine_bridge.c` | 把 IPC server 的 hooks（prepare/commit/clear/abort/counts/ptp_state/lane_stats/tx_start/tx_stop/tx_status）桥接到引擎 |
| `engine/tx_session.c` `engine/tx_port_map.c` `engine/tx_backend_live_ingest.c` | TX 参数校验 + 状态机、捕获端口映射（稳定身份优先）、live_ingest 数据面入口与启停 |
| `engine/capability.c` | 节点能力探测与上报（DRM 连接器/plane、V4L2 输入口、MTL/MPP/PTP 状态，与 TX 共用同一套端口扫描表） |
| `ipc/ipc_server.c` | Unix socket IPC 服务 + `hello` 握手 |
| `ipc/state_machine.c` | `configure/prepare/commit/abort/clear/query_state` 六状态机 |

---

## 6. 共享版本契约：`dvs_common`

dvs_media 与 dvs_agent **同包发布**，版本契约在 `app/dvs_common/include/dvs_node_bundle.h`：
- `DVS_NODE_BUNDLE_VERSION`（包版本，当前 `"1"`）
- `DVS_MEDIA_IPC_VERSION`（IPC 协议代，当前 `1`）

`dvs_media_main.c` 在 `--version` 时打印二者，IPC 握手时双方比对，避免版本错配。该头文件用纯 `#define`，C（dvs_media）与 C++（dvs_agent）均可消费。

---

## 7. 编译与依赖

- 独立工程：`cd app/dvs_media && meson setup build && ninja -C build`
- **必须在编译服务器**构建（依赖 MTL / Rockchip MPP / libdrm / ALSA / FFmpeg / json-c）。
- MPP 为**必需**：meson 探测不到会直接 `error()` 退出。
- RGA 缺席时 TX 后端不链接，整机仍可构建并跑 RX 与单测，TX 只报 `configured+pending`；单板 input 角色需要 RGA。
- 依赖声明：`mtl`、`rockchip_mpp`、`libdrm`、`alsa`、`json-c`、`libavformat/avcodec/avutil`、`threads`，有 TX 后端时加 `librga`。

---

## 8. 注意事项与潜在坑

1. **AGENTS.md 描述与代码不一致（重要）**：AGENTS.md 称 file_playout 的 `shared/` "each app carries its own copy"（各自持有一份副本）。但 dvs_media 实际是 **in-tree 直接 `#include`/编译 file_playout 的 `shared/` 与 `rx/` 源码**，并非复制。涉及这两处改动时，影响面会跨 app，需同步验证 file_playout 与 dvs_media。建议修正 AGENTS.md 或统一为复制。

2. **rx_compositor 来源**：dvs_media 复用的是 `rk3588_file_playout/rx/rx_compositor.c`，不要与 `io/drm_renderer.c` 或 file_playout 的 `shared/drm_renderer.c` 混淆（AGENTS.md 已提示存在近似重复模块）。改 `dvs_media` 的渲染/合成行为时确认目标文件是 `rx/rx_compositor.c`。

3. **强耦合 donor 内部头**：`engine_ctx.c` 直接引用 `file_playout_config.h` / `display_layout.h`（后者已上移 `rk3588_common`），任一 donor 重构这些头都会直接破坏 dvs_media。

4. **TX 已闭环，余量是 HW 冒烟**：output 回归、input 出流、output→input→output 往返切换需在单板验证；`dvs_tx_set_backend` 的 fake 后端只留在 `tests/test_tx_session.c` 单测里。

5. **donor 源码跨 app 引用**：`rk3588_common/src/` 同时被三处引用；`rk3588_file_playout/shared/` 余下部分仍被 file_playout 自己与 dvs_media 引用。新增引用前先确认归属，避免回搬。

---

## 9. 集成关系图（文本）

```
                 rk3588_common (static: ring_buffer/slab/timer)
                          ▲
                          │ in-tree 编译
                          │
   rk3588_file_playout ───┤  rx/*  shared/*  tx/mp4_demux  file_playout_config
                          │  (RX 数据面 + 配置，被借用)
                          │
   rk3588_live_ingest ────┤  (TX 数据面，经 dvs_tx + 后端入口接入)
                          │
                  ┌───────┴───────────────┐
                  │     dvs_media         │
                  │  engine/  (控制面)     │
                  │  ipc/     (IPC/状态机) │
                  │  dvs_media_main.c      │
                  └───────┬───────────────┘
                          │ 同包发布 + IPC 握手
                          ▼
                     dvs_agent / dvs_platform
```
