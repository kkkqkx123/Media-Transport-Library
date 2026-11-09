# RK3588 DRM 显示验证方案

目标：在 RK3588 板（HDMI OUT 已接显示设备、默认 Debian 桌面）上验证 DRM 显示路径，
覆盖设备枚举、模式设置、NV12 overlay 平面、缩放、多平面合成、页面翻转与恢复，
为 `rk3588_playout_rx` 的 `drm_renderer` 与 `rx_compositor` 提供冒烟与回归依据。

## 1 前置与环境

- 显示器已通过 HDMI OUT 连接，桌面系统正常显示。
- DRM 节点 `/dev/dri/card0` 可用；需要 root 或当前用户在 `video` 组。
- 若 X/Wayland 占用 DRM master，`drmSetMaster` 会失败；冒烟测试建议切换到文本 VT
  （Ctrl+Alt+F3）或临时停用显示管理器，测试结束后恢复。
- 本方案不依赖 MTL/hugepage，仅依赖 libdrm 与内核 DRM 驱动。

## 2 验证分层

### L1 设备枚举（无显示副作用）

- 打开 DRM 设备，设置 `DRM_CLIENT_CAP_UNIVERSAL_PLANES`。
- 枚举已连接 connector、可用 mode、CRTC、plane。
- 确认存在 NV12-capable overlay plane（RK3588 Esmart/Cluster 平面）。
- 通过标准：connector connected，mode 列表非空，至少一个 NV12 overlay plane。

### L2 单 overlay 冒烟

- 创建黑背景 XRGB dumb buffer，`SetCrtc` 全屏。
- 创建 NV12 dumb buffer，填充彩条图案，`SetPlane` 缩放至 PiP 矩形。
- 运行 N 秒，可选动画移动矩形。
- 通过标准：SetCrtc/SetPlane 返回 0，画面上可见黑底 + 彩条 PiP，无内核错误。

### L3 多平面合成（可选，进阶）

- 使用 2-4 个 NV12 overlay plane，分别提交不同彩条/裁剪区域，验证 `drmModeSetPlane`
  多平面同帧合成与 z-order。
- 通过标准：各 plane 正常显示，无 flicker/撕裂。

### L4 模式与 plane 恢复

- 测试退出前恢复原 CRTC mode 与 plane 状态（`drmModeSetCrtc` saved crtc、
  `drmModeSetPlane(... fb_id=0 ...)`）。
- 通过标准：退出后桌面正常恢复，无残留 overlay。

### L5 集成验证（file_playout_rx）

- 运行 `file_playout_rx` 接收 MTL 流或离线解码，确认 DRM 呈现帧数与解码帧数匹配，
  无 `drm` 错误、无 underrun。
- 通过标准：日志中 render 增量 ≈ dec 增量，无 DRM ioctl 失败。

## 3 测试文件

- `debug/tests/drm_smoke.c`：单文件 C 程序，支持 `--list-planes-only`（L1）与默认显示冒烟（L2）。
  编译：`gcc debug/tests/drm_smoke.c -o ~/code/mtl/bin/drm_smoke -ldrm`
- 可扩展：多平面版本可另建 `debug/tests/drm_multilayer.c`（按 L3 需求补充）。

## 4 判定与回归

- L1/L2 自动判定依赖退出码与 stdout `PASS:` 行。
- L3/L4 需人工视觉确认 + 退出码。
- L5 依赖日志解析与帧数比。
- 每轮记录：connector/mode、plane id 列表、NV12 plane id、SetCrtc/SetPlane 结果、运行时长。

## 5 风险

- DRM master 冲突：桌面占用时 SetMaster 失败，需 VT 或停显示管理器。
- plane 能力差异：RK3588 部分 overlay plane 不支持 NV12 或缩放，需枚举确认。
- 模式恢复失败会导致桌面异常，务必保留 saved CRTC 并在信号处理中恢复。
