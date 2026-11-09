# RK3588 DRM 冒烟测试使用说明

## 1 编译

板端执行（持久目录 `~/code/mtl`）：

```bash
mkdir -p ~/code/mtl/bin
gcc ~/code/mtl/src/../debug/tests/drm_smoke.c -o ~/code/mtl/bin/drm_smoke -I/usr/include/libdrm -ldrm
# 或仓库路径
gcc debug/tests/drm_smoke.c -o ~/code/mtl/bin/drm_smoke -I/usr/include/libdrm -ldrm
```

依赖：`libdrm-dev`（板上通常已安装，头文件 `/usr/include/xf86drm.h`）。

## 2 L1 枚举（无显示副作用）

```bash
~/code/mtl/bin/drm_smoke --list-planes-only
```

预期输出包含 connector、mode、plane 列表与 `PASS: N plane(s), NV12 overlay available`。

## 3 L2 显示冒烟

建议从文本 VT 运行以避免 DRM master 冲突：

```bash
# 切换到 VT3 (Ctrl+Alt+F3) 后
sudo ~/code/mtl/bin/drm_smoke --duration 10
```

可选参数：
- `--device /dev/dri/card0`
- `--connector HDMI-A`
- `--mode 1920x1080@60`
- `--pip-x 100 --pip-y 100 --pip-w 640 --pip-h 360`
- `--no-animate`
- `--verbose`

预期：黑屏背景 + 彩条 PiP，10 秒后退出并恢复桌面。

## 4 常见问题

- `drmSetMaster` 失败：在桌面会话下运行，切换 VT 或停用显示管理器。
- 无 NV12 plane：确认内核 DRM 驱动与 `DRM_CLIENT_CAP_UNIVERSAL_PLANES`。
- 退出后桌面未恢复：手动重启显示管理器或重启板卡；检查 saved CRTC 恢复路径。

## 5 集成验证

运行 `file_playout_rx` 接收流，观察 `RX stats` 与 DRM 呈现，确认无 DRM ioctl 错误。
