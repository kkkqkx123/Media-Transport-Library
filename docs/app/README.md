# docs/app — 新增应用模块文档

记录本仓库在 `app/` 下新增的两个 RK3588 应用模块的功能与结构。它们基于 MTL
传输层，在 Rockchip RK3588 (ARM) 平台上实现端到端 ST 2110 媒体链路。

| 文档 | 内容 |
|------|------|
| [`rk3588_end2end.md`](./rk3588_end2end.md) | 低延迟直播链路：V4L2 采集 → MPP 编码 → MTL 发送/接收 → MPP 解码 → DRM 渲染 |
| [`rk3588_mp4_dist.md`](./rk3588_mp4_dist.md) | MP4 文件分发：FFmpeg 解复用 → ST2110-22/30 发送 → MPP 解码 + DRM 显示 + ALSA 播放 |

## 共同特征

- **独立 meson 工程**：各自带有自己的 `meson.build`，**未接入** `app/meson.build`，
  需在各自目录内单独构建（`meson setup build && ninja -C build`）。
- **平台依赖**：Rockchip MPP（硬编解码）、librga、libdrm、V4L2、ALSA、json-c；
  库端依赖已安装的 MTL（`mtl` pkg-config）。TX 侧（`mp4_dist_tx`）可在 x86 上编译，
  RX 侧需要 RK3588 + MPP。
- **Pacing**：ARM 平台无 E810 硬件限速，使用软件 pacing（TSC）。
- **共享代码**：两个应用各自带一份近似重复的基础模块（MPP 解码器、DRM 渲染、
  ring buffer、slab pool、timer），当前未抽取公共库，修改时注意两处同步。
- 单测位于各自 `tests/` 目录；`mp4_dist` 另有 Python 日志校验脚本与单板/双板
  验证脚本。
