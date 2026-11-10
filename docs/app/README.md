# docs/app — 新增应用模块文档

记录本仓库在 `app/` 下新增的两个 RK3588 应用模块的功能与结构。它们基于 MTL
传输层，在 Rockchip RK3588 (ARM) 平台上实现端到端 ST 2110 媒体链路。

| 文档 | 内容 |
|------|------|
| [`rk3588_live_tx.md`](./rk3588_live_tx.md) | RK3588 编码发送端（TX-only）：V4L2 采集 → MPP 编码 → MTL 发送；含文件帧源插件 |
| [`rk3588_playout_rx.md`](./rk3588_playout_rx.md) | RK3588 解码接收端（RX-only）：MTL 接收 → MPP 解码 + DRM 显示 + ALSA 播放 |
| [`hardware_usage.md`](./hardware_usage.md) | 两个应用对 RK3588 各硬件（V4L2/MPP/RGA/DRM/ALSA/DMA-BUF/MTL/PTP）的使用情况、零拷贝路径与软件回退分析 |
| [`mtl_usage.md`](./mtl_usage.md) | rk3588 两个应用与 dvs（dvs_media/dvs_agent/dvs_platform）如何使用 MTL：初始化、ST22/ST30 会话、framebuffer、PTP/pacing、API 清单与缺口 |
| [`frontend_prototype_integration.md`](./frontend_prototype_integration.md) | `ref/` 两个前端原型（平台后台 / 节点本机后台）与 `docs/req` 需求的对应关系，及其与现有 rk3588 app 层的集成设计方案 |
| [`dvs_agent_frontend_mapping.md`](./dvs_agent_frontend_mapping.md) | dvs_agent 本机前端（node-admin）功能映射到 dvs / rk3588 app 功能的分析 |
| [`dvs_platform_frontend_mapping.md`](./dvs_platform_frontend_mapping.md) | dvs_platform 平台前端功能映射到 dvs / rk3588 app 功能的分析 |
| [`dvs_node_analysis.md`](./dvs_node_analysis.md) | `dvs_media` / `dvs_agent` 功能现状与 `docs/req` 需求差距分析；IPC 方式评估与网络通信（平台 WSS 注册心跳）补充建议 |
| [`dvs_media.md`](./dvs_media.md) | `dvs_media` 节点媒体引擎功能分析：input/output 角色、IPC prepare/commit 状态机、帧锚定提交、准入/音频仲裁/TX 会话与能力上报 |
| [`dvs_agent.md`](./dvs_agent.md) | `dvs_agent` 节点代理功能分析：注册/心跳与 WS 通道、六 scope 配置状态机、平台布局翻译、维护操作与安全门 |
| [`dvs_platform.md`](./dvs_platform.md) | `dvs_platform` 平台服务功能分析：布局像素拆分、OpCoordinator 跨节点屏障与回退、升级与退役墓碑 |
| [`dvs_url.md`](./dvs_url.md) | dvs URL 设置一览：两端源站/端口/静态根、全部 HTTP 与 WS 路径、端间调用、环境变量与反向代理规则 |

## 依赖分析

- [`dep/sqlite.md`](./dep/sqlite.md) — `app/dvs_common/third_party/sqlite`（SQLite amalgamation）在 dvs_agent / dvs_platform 中的使用与作用。

## 共同特征

- **独立 meson 工程**：各自带有自己的 `meson.build`，**未接入** `app/meson.build`，
  需在各自目录内单独构建（`meson setup build && ninja -C build`）。
- **平台依赖**：Rockchip MPP（硬编解码）、librga、libdrm、V4L2、ALSA、json-c、FFmpeg；
  库端依赖已安装的 MTL（`mtl` pkg-config）。两个应用均为 RK3588 交叉编译专用，无 x86 回退。
- **Pacing**：ARM 平台无 E810 硬件限速，使用软件 pacing（TSC）。
- **共享代码**：`app/rk3588_common/` 已抽取 A/B 档基础模块（ring buffer、slab pool、
  timer、codec_plugin.h、pipeline_common.h），`end2end` 与 `file_playout` 已引用。C/D 档
  （drm_renderer、mpp_decoder、drm_mode、display_layout、ptp_time 等）与 end2end 增量
  （dma_allocator、rga_converter、mtl_rx_bridge 等）的合并**尚未完成**。`dvs_media`
  当前经 `meson.build` **in-tree 编译直接复用** `rk3588_playout_rx` 的 `rx/`、`shared/`（含
  `mp4_demux`、`extradata_handler`）与 `rk3588_common`（非拷贝）；input 角色数据面经
  `engine/tx_backend_live_ingest.c` 复用 `rk3588_live_tx` 的 donor 链。修改这两个应用的这些
  目录会直接影响 dvs_media 的构建与行为。
- 单测位于各自 `tests/` 目录；`file_playout` 另有 Python 日志校验脚本与单板/双板
  验证脚本。
