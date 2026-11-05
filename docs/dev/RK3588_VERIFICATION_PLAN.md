# RK3588 功能验证方案

> 最后更新：2026-11-02
> 适用：在 RK3588 板端对 MTL 库与 `rk3588_live_tx` / `rk3588_playout_rx` 应用做功能完整性验证。
> 部署前提见 `docs/dev/RK3588_DEPLOYMENT.md`，交叉编译链路见 `docs/archive/RK3588_CROSS_COMPILE_PROGRESS.md`。

---

## 1. 板端现状与前置修复

### 1.1 已确认的环境

| 项目 | 状态 |
|---|---|
| 板端 IP | `192.168.80.202`，RK3588，Debian 11，内核 5.10.226 |
| MTL 库 | `/usr/lib/aarch64-linux-gnu/libmtl.so` 26.9.0.DEV |
| 应用 | `/opt/mtl-rk3588/bin/` 下 `live_ingest`、`file_playout_tx`、`file_playout_rx`、`rx_minimal`、`test_*` |
| 网口 | `enP2p33s0` UP；`enP3p49s0` 无载波 DOWN |
| 摄像头 | `/dev/video0` = `rk_hdmirx`，接入 HDMI 时钟视频后为 1920×1080@60 BGR3 |
| 显示 | HDMI-A-1/2、DP-1 全部 disconnected；仅 `Writeback-1` connected |
| 音频 | 5 个 ALSA 设备 |
| Hugepages | 需手动设为 512×2MB |
| MtlManager | 未安装（AF_XDP 单实例模式可用） |
| DPDK 插件目录 | 只保留 `librte_mempool_ring.so`，避免 EAL 重复注册 panic |
| MTL mempool ops | 板端 DPDK 未编译 stack ops，MTL 默认 ops 改为 `ring_mp_mc` |

### 1.2 前置修复命令

```bash
echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
mkdir -p /usr/lib/dpdk/pmds-26.2
cp /home/kkkqkx/mtl/sysroot/usr/lib/dpdk/pmds-26.2/librte_mempool_ring.so* /usr/lib/dpdk/pmds-26.2/
rm -f /usr/lib/dpdk/pmds-26.2/librte_bus_pci.so* \
      /usr/lib/dpdk/pmds-26.2/librte_bus_vdev.so* \
      /usr/lib/dpdk/pmds-26.2/librte_net_null.so*
ldconfig
```

---

## 2. 分层验证

### L0：库与运行时依赖

```bash
file /usr/lib/aarch64-linux-gnu/libmtl.so
ldd /usr/lib/aarch64-linux-gnu/libmtl.so | grep -E "not found|libxdp|libbpf"
```

通过标准：aarch64，无 `not found`。

### L1：组件级自测（无需显示/网络）

```bash
cd /opt/mtl-rk3588/bin
./test_hal
./test_sps_vui
./test_display_layout
./test_drm_mode
./test_media_clk
./test_e2e_stamp
```

通过标准：全部 `0 failed` / `PASS`。

### L2：MTL 传输初始化（AF_XDP）

```bash
cd /opt/mtl-rk3588/bin
timeout 8 ./rx_minimal_afxdp
```

通过标准：`mtl_init` 成功，出现 mempool `succ`，无 `Operation not supported` / `dev_eal_init fail`。

### L3：端到端数据面

- TX 与 RX 使用同一网口 `enP2p33s0`，IP 分别设为板端自身或对端 PC。
- 使用 `tcpdump -i enP2p33s0 udp port 20000`、MTL stats 日志、帧计数与吞吐验证。

通过标准：RX 帧计数递增、无持续丢包、吞吐匹配视频码率。

### L4：解码 / 显示 / 音频

- 接入 HDMI 显示器后重跑 `test_io`、`test_codec`、`test_pipeline`。
- 无显示器时可使用 `Writeback-1` 连接器做无头 DRM 路径验证。
- `file_playout_rx` 应出现 MPP 解码成功、DRM pageflip、ALSA 播放。

### L5：双机 PTP

1. 板端与 PC 安装 `linuxptp`，运行 `set_tai_offset`，同步 TAI。
2. PC（192.168.80.211）启动 PTP master + `file_playout_tx`。
3. 板端启动 PTP slave + `file_playout_rx`。
4. 检查 `PTP mode=external`、`av_sync clock=ptp`、PTS 单调。

### L6：稳定性与性能

```bash
./file_playout_rx 300 --config config/rx_file_playout.json --fps 23.976
```

监控 CPU、丢包、解码帧数、playout 延迟与 DRM 刷新率。

---

## 3. 当前阻塞项

| 阻塞项 | 说明 |
|---|---|
| MtlManager 未安装 | AF_XDP 多队列/共享模式不可用，`live_ingest` 的 AF_XDP 路径需要它 |
| `InTheGrey.mp4` 未部署 | 板端无法运行 `file_playout_tx` 全链路 |
| 无 HDMI 显示器 | DRM 渲染测试无法在物理显示器上验证 |
| linuxptp 未安装 | 双机 PTP 验证前需安装 |