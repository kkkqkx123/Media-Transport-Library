# RK3588 验证结果归档

> 最后更新：2026-11-02
> 板端：192.168.80.202，RK3588，Debian 11，内核 5.10.226
> 验证方案见 `docs/dev/RK3588_VERIFICATION_PLAN.md`

---

## 1. 环境修复结果

| 修复项 | 结果 |
|---|---|
| Hugepages 设为 512×2MB | ✅ 成功 |
| DPDK 插件目录只保留 mempool_ring | ✅ EAL 不再 panic |
| MTL mempool ops 从 `stack` 改为 `ring_mp_mc` | ✅ `mt_mempool_create_by_ops` 返回 succ |
| `libmtl.so` 重新部署 | ✅ 板端运行时使用新库 |
| HDMI 输入接入 | ✅ `/dev/video0` 识别为 1920×1080@60 BGR3 |

---

## 2. L0：库与运行时依赖

- `file libmtl.so`：ELF 64-bit ARM aarch64
- `ldd`：所有依赖已解析，无 `not found`

**结果：✅ 通过**

---

## 3. L1：组件级自测

| 测试 | 结果 |
|---|---|
| `test_hal` | ✅ 0 failures |
| `test_sps_vui` | ✅ all passed |
| `test_display_layout` | ✅ 0 failed |
| `test_drm_mode` | ✅ 0 failed |
| `test_media_clk` | ✅ 0 failed |
| `test_e2e_stamp` | ✅ OK |
| `test_codec` | ❌ `drm: connector HDMI-A-1 not found`（无物理显示器） |
| `test_io` | ❌ `drm: connector HDMI-A-1 not found` |
| `test_pipeline` | ❌ `rx_pipe: renderer create failed`，但 TX 编码路径已就绪（`tx_pipe: 1920x1080@60fps ... ready`） |

---

## 4. L2：MTL 传输初始化

- `rx_minimal_afxdp`（`native_af_xdp:enP2p33s0`）运行结果：
  - EAL 初始化成功
  - `mt_mempool_create_by_ops`：`T_P0_SYS_0` succ
  - `dev_if_init_tx_queues` / `dev_if_init_rx_queues` succ
  - `mtl_init` 成功，单实例模式运行

**结果：✅ 通过**

---

## 5. L3：端到端数据面

- 板端无 `InTheGrey.mp4`，且 `e2e_app` 的 AF_XDP 路径需要 MtlManager，因此未完成完整音视频端到端发送。
- MTL AF_XDP 单实例初始化与队列创建已验证通过。

**结果：⚠️ 部分验证（传输初始化通过，完整媒体流未跑通）**

---

## 6. L4：解码 / 显示 / 音频

- HDMI 输入接入后，`test_pipeline` 的 V4L2 采集从 640×480 提升为 1920×1080，RGA/MPP 编码初始化成功。
- 由于无物理 HDMI 显示器，DRM 渲染器创建失败，解码/显示链路未能完整验证。

**结果：⚠️ 编码路径通过，显示路径未验证**

---

## 7. L5 / L6

- 未执行：板端缺少 `linuxptp`、`InTheGrey.mp4`、物理显示器。

---

## 8. 结论

- 已验证通过：库加载、组件测试（6 项）、MTL AF_XDP 传输初始化、V4L2 1080p60 采集、RGA/MPP 编码初始化。
- 待补齐：MtlManager、MP4 测试片源、HDMI 显示器、linuxptp，以完成 e2e_app / mp4_dist 全链路与双机 PTP 验证。