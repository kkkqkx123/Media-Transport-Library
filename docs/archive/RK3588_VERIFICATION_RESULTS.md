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

- 板端无 `InTheGrey.mp4`，且 `live_ingest` 的 AF_XDP 路径需要 MtlManager，因此未完成完整音视频端到端发送。
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
- 待补齐：MtlManager、MP4 测试片源、HDMI 显示器、linuxptp，以完成 live_ingest / file_playout 全链路与双机 PTP 验证。
---

## 9. 补充验证：内核后端 TX/RX

### 9.1 file_playout_tx（kernel backend）

- 配置：`tx_port=kernel:enP2p33s0`，TX/RX IP 均为 `192.168.80.202`
- 结果：MTL 初始化成功，`st22_tx_create` / `st30_tx_create` 成功，链路发送约 4.06 Mb/s、11238 packets
- 说明：`config_parse_json: open json file kahawai.json fail` 为非致命默认配置缺失

**结果：✅ TX 数据面通过（内核后端）**

### 9.2 rx_minimal（kernel backend）

- 结果：MTL 与 mempool 初始化成功；在同机回环条件下未收到帧（0 frames）
- 说明：内核后端/AF_XDP 的本机回环接收可能受限，需对端设备或抓包进一步确认

**结果：⚠️ RX 初始化通过，回环收帧未验证**

---

## 10. MtlManager 补充与 AF_XDP 可用性

### 10.1 构建结果

- 使用板端原生 `g++-10` 成功编译 `MtlManager`（含 XDP 支持）。
- 安装 `xdp-dispatcher.o` 到 `/lib/bpf/`。
- MtlManager 可启动并监听客户端。

### 10.2 AF_XDP 运行结果

- `live_ingest tx` 仍失败：`register fail` / `AF_XDP backend must run with MTL Manager!`
- Manager 日志显示：`Underlying driver does not support XDP in native mode`
- 结论：板端网卡驱动（`enP2p33s0`）在当前 5.10 内核下不支持 native XDP，因此 AF_XDP 路径不可用。

### 10.3 可用替代路径

- 使用 `kernel:` 后端进行 MTL 验证，不依赖 MtlManager 与 XDP 驱动支持。
- `file_playout_tx` 已在内核后端成功发送 ST22/ST30 数据。

---

## 11. AF_XDP 不可用根因分析

### 11.1 现象

- `mt_dev_xdp_init`: `Underlying driver does not support XDP in native mode`
- MtlManager 尝试 native attach 失败，skb fallback 也失败。

### 11.2 原因

- 板端网卡驱动为 `yt6801`（PCI 1F0A:6801）。
- 该驱动源码（`drivers/net/ethernet/yt6801`）未实现 `ndo_bpf` / `ndo_xdp_xmit`，因此不支持 XDP。
- 内核配置已启用 `XDP_SOCKETS` / `BPF_SYSCALL`，但驱动层不具备 XDP 能力。
- `rk_gmac-dwmac`（eth0）同样未实现 XDP；仅 USB 网卡驱动支持通用 XDP。

### 11.3 结论

- 这是**驱动/硬件能力限制**，不是内核配置问题。
- `patches/rk3588-5.10` 的 0001 仅启用 AF_XDP/BPF 配置；0002 引入 YT6801 驱动但未添加 XDP 支持。
- 若要启用 AF_XDP，需要为 YT6801 驱动实现 XDP hook，或更换支持 XDP 的 NIC/驱动。
- 当前推荐验证路径：使用 `kernel:` 后端，不依赖 AF_XDP。


---

## 12. ALSA HDMI 音频采集验证

- 设备：`hw:3,0` = `rockchip,hdmiin`
- 方法：`ffmpeg -f alsa -i hw:3,0 -t 4 ...`
- 结果：成功采集 4s 音频，非静音（mean_volume -20.8 dB，max_volume -2.3 dB）
- 说明：`arecord` 默认 U8 格式不可用，需显式指定 S16_LE/S24_LE/S32_LE
- 结论：✅ ALSA HDMI 输入音频采集通过
