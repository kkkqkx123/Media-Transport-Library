# docs/app — 新增应用模块文档

记录本仓库在 `app/` 下新增的两个 RK3588 应用模块的功能与结构。它们基于 MTL
传输层，在 Rockchip RK3588 (ARM) 平台上实现端到端 ST 2110 媒体链路。

| 文档 | 内容 |
|------|------|
| [`rk3588_live_ingest.md`](./rk3588_live_ingest.md) | 低延迟直播链路：V4L2 采集 → MPP 编码 → MTL 发送/接收 → MPP 解码 → DRM 渲染 |
| [`rk3588_file_playout.md`](./rk3588_file_playout.md) | MP4 文件分发：FFmpeg 解复用 → ST2110-22/30 发送 → MPP 解码 + DRM 显示 + ALSA 播放 |
| [`hardware_usage.md`](./hardware_usage.md) | 两个应用对 RK3588 各硬件（V4L2/MPP/RGA/DRM/ALSA/DMA-BUF/MTL/PTP）的使用情况、零拷贝路径与软件回退分析 |
| [`mtl_usage.md`](./mtl_usage.md) | rk3588 两个应用与 dvs（dvs_media/dvs_agent/dvs_platform）如何使用 MTL：初始化、ST22/ST30 会话、framebuffer、PTP/pacing、API 清单与缺口 |
| [`frontend_prototype_integration.md`](./frontend_prototype_integration.md) | `ref/` 两个前端原型（平台后台 / 节点本机后台）与 `docs/req` 需求的对应关系，及其与现有 rk3588 app 层的集成设计方案 |
| [`dvs_agent_frontend_mapping.md`](./dvs_agent_frontend_mapping.md) | dvs_agent 本机前端（node-admin）功能映射到 dvs / rk3588 app 功能的分析 |
| [`dvs_platform_frontend_mapping.md`](./dvs_platform_frontend_mapping.md) | dvs_platform 平台前端功能映射到 dvs / rk3588 app 功能的分析 |
| [`dvs_node_analysis.md`](./dvs_node_analysis.md) | `dvs_media` / `dvs_agent` 功能现状与 `docs/req` 需求差距分析；IPC 方式评估与网络通信（平台 WSS 注册心跳）补充建议 |

## 共同特征

- **独立 meson 工程**：各自带有自己的 `meson.build`，**未接入** `app/meson.build`，
  需在各自目录内单独构建（`meson setup build && ninja -C build`）。
- **平台依赖**：Rockchip MPP（硬编解码）、librga、libdrm、V4L2、ALSA、json-c；
  库端依赖已安装的 MTL（`mtl` pkg-config）。TX 侧（`file_playout_tx`）可在 x86 上编译，
  RX 侧需要 RK3588 + MPP。
- **Pacing**：ARM 平台无 E810 硬件限速，使用软件 pacing（TSC）。
- **共享代码**：`app/rk3588_common/` 已抽取 A/B 档基础模块（ring buffer、slab pool、
  timer、codec_plugin.h、pipeline_common.h），`end2end` 与 `file_playout` 已引用。C/D 档
  （drm_renderer、mpp_decoder、drm_mode、display_layout、ptp_time 等）与 end2end 增量
  （dma_allocator、rga_converter、mtl_rx_bridge 等）的合并**尚未完成**。`dvs_media`
  当前直接经 `meson.build` 复用 `rk3588_file_playout/shared/` 与 `rk3588_file_playout/rx/` 的
  副本（即 S0 规划的 fallback 路径），并接受与 `file_playout` 的耦合；公共库收敛见
  `docs/plan/DVS_REMAINING_TASKS.md` T5。修改时注意 `file_playout` 侧与 `dvs_media` 侧的同步。
- 单测位于各自 `tests/` 目录；`file_playout` 另有 Python 日志校验脚本与单板/双板
  验证脚本。
