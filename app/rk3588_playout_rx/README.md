# file_playout — MTL ST2110 解码接收端（RK3588）

MTL ST2110-22（视频）/ ST2110-30（音频）接收；MPP 硬解码 + DRM 显示 + ALSA 播放；支持单流与多路合成（`rx_compositor`）、播放缓冲、AV 同步。本包只收流，不产流；发送端由编码包 `rk3588_live_tx` 经网络供给，两侧帧率与分辨率显式对齐。

参考流参数探针固定用仓库内 **`InTheGrey.mp4`**（1920×1080 @ 23.976fps，路径为 `../InTheGrey.mp4`）做期望流描述；实际网流由编码端产生，`--fps/--width/--height` 必须与发送端一致。

## 应用架构

```
编码端 (rk3588_live_tx)              网络 UDP                  接收端 (file_playout_rx)
─────────────────────────              ───────────              ─────────────────────────
V4L2 → RGA → MPP编码 → MTL TX  ──►  ST2110-22 视频码流  ──►  ST22 RX → bridge
                                                          → MPP 解码 → DRM
                                                          → playout → A/V sync
                                     ST2110-30 音频 PCM  ──►  ST30 RX → ALSA
```

| 组件 | 说明 |
|------|------|
| `file_playout_rx` | 接收端：MTL 收包 + MPP/DRM/ALSA（需 RK3588 + MPP） |
| `rx/` | MTL 收包、桥接、播放缓冲、时钟同步、合成管线 |
| `shared/` | 解码器、渲染器、复用探针（`mp4_demux`，参考流描述）、附加数据拼接、统计与时间轴 |
| `tests/` | 单板验证脚本与离线检查工具 |

## 依赖

| 依赖 | 说明 |
|------|------|
| MTL ≥ 26.01 | 先编译安装主工程：`./build.sh release`（交叉编译服务器） |
| FFmpeg | libavformat / libavcodec / libavutil（探针） |
| json-c | 配置解析 |
| Rockchip MPP | 硬解码（必需） |
| libdrm | HDMI/DRM 输出（必需） |
| ALSA | 音频播放（必需） |
| libbpf | MTL/DPDK EAL 运行时 |
| MtlManager + hugepages | 使用 `native_af_xdp:` 时必需 |

本工程为 **RK3588 交叉编译专用**，不提供桌面构建分支。

```bash
# Ubuntu/Debian 示例
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libjson-c-dev libbpf0
```

## 编译构建

本工程为 **独立 meson 项目**，需先安装 MTL 主库。**在编译服务器上构建**：

```bash
cd app/rk3588_playout_rx
meson setup build
ninja -C build

# 产物
ls build/file_playout_rx
```

## 配置文件

| 文件 | 场景 | 运行主机 |
|------|------|----------|
| `rx_dual_host.json.example` | 双机接收 | RK3588 |

默认 UDP：**视频 20000**，**音频 20002**（未在 JSON 中写出时使用程序默认值）。

网卡名请按 `ip link` 修改。**RX 的 `--fps` 必须与发送端一致**。

## PTP 时间同步

多机场景需发送与接收共享同一 PTP 时间域，默认使用 **外部 PTP**（linuxptp + `ptp_get_time_fn` → `CLOCK_TAI`）。

### 前置条件

1. **linuxptp ≥ 2.0**
2. 内核 TAI offset：`sudo ../../tools/set_tai_offset/build/set_tai_offset`
3. 关闭 NTP：`sudo systemctl stop chrony systemd-timesyncd`

### 启动 PTP（双机单播）

```bash
# 终端 1 — PC/Master
cd app/rk3588_playout_rx
chmod +x scripts/start_ptp_dual_host.sh
./scripts/start_ptp_dual_host.sh master enp1s0

# 终端 2 — RK (Slave)
./scripts/start_ptp_dual_host.sh slave eth0 192.168.80.213
```

配置文件：`config/ptp/ptp4l-master-unicast.conf`、`config/ptp/ptp4l-slave-unicast.conf`。

### 配置项

| JSON / CLI | 说明 |
|------------|------|
| `ptp_mode`: `"external"` | 默认；linuxptp 同步后的 CLOCK_TAI |
| `ptp_mode`: `"builtin"` | MTL 内置 PTP（需 NIC HW timesync） |
| `ptp_mode`: `"monotonic"` | 无 PTP；RX 用 monotonic 相对锚定 |
| `ptp_utc_offset` | GM 为 UTC 域时从 TAI 减去的秒数（通常 0） |
| `--ptp-mode` / `--ptp-utc-offset` | CLI 覆盖 |

### ST 2110-21 pacing profile

| JSON / CLI | 说明 |
|------------|------|
| `st21_pacing`: `"tsc_narrow"` | **默认**；TSC narrow gapped，最接近 ST 2110-21 Narrow，广播互通首选 |
| `st21_pacing`: `"tsc"` | TSC 线性，近似 Medium，接收端宽松时可用 |
| `st21_pacing`: `"auto"` | MTL 自动（历史行为：AF_XDP 下回退 TSC） |
| `st21_pacing`: `"rl"` | 硬件限速（RK3588 无对应 NIC，保留给未来 10 GbE） |
| `--pacing <way>` | CLI 覆盖 |

说明：ST22 frame-level 会话在 MTL 库内会被强制从 RL 降为 TSC（每帧包数不固定），
`rl` 档对 ST22 实际运行在 TSC；`tsc_narrow` 不受影响，是推荐档。
对接专业接收机后用 `tests/verify_vrx_margin.py` 校验 VRX 余量：

```bash
python3 tests/verify_vrx_margin.py --require-tx run.log
```

### 代码路径

- `rk3588_common/src/ptp_time.c` — MTL `ptp_get_time_fn` 读 TAI
- `rx/media_clk.c` — ST22 MEDIA_CLK 首帧锚定到 `mtl_ptp_read_time()`
- `rx/av_sync.c` — playout 调度与视频 PTS 同域（PTP）
- `rx/mtl_rx_audio.c` — ST30 使用 `timestamp_first_pkt` / `st10_get_tai()`

## 运行说明

命令格式：`file_playout_rx [秒数] [--config FILE] [CLI 覆盖...]`

### 场景：单板（RK3588 发送端 + 接收端两进程）

先起 RX，等 2 秒，再起编码端发送（帧率与分辨率两侧对齐）：

```bash
cd app/rk3588_playout_rx

# 接收（另开终端）
./build/file_playout_rx 120 --config config/rx_dual_host.json.example --fps 60 --width 1920 --height 1080 &
sleep 2
# 发送（编码包，需 HDMI 信号）
../rk3588_live_tx/build/live_ingest 120 --udp 15000 --dip 192.168.80.212
```

可选 trace（AU 对齐分析）：

```bash
export FILE_PLAYOUT_AU_TRACE=/tmp/dual
export FILE_PLAYOUT_DUMP_RX=/tmp/rx_capture.h264
export FILE_PLAYOUT_DUMP_RX_IDX=/tmp/rx_capture.idx
```

### CPU / lcore 说明（RK3588）

| CPU | 类型 | 典型用途 |
|-----|------|----------|
| 0–3 | A55 | bridge、audio |
| 4–7 | A76 | decode、render |

- **`mtl_lcores_*`**：MTL/DPDK worker 列表，**不含 CPU 0**（0 为 EAL main）

## 故障排查

| 现象 | 排查 |
|------|------|
| MTL init 失败 | AF_XDP：检查 MtlManager、hugepages；调试可用 `kernel:<ifname>` |
| `libbpf.so.0` 缺失 | `sudo apt install libbpf0` |
| `EAL: lcore N unavailable` | 减小 `mtl_lcores_*` |
| RX 无画面 | 确认 `rx_sip` = 发送端 `lip`；`tcpdump -i eth0 udp port 15000` |
| fps 不匹配 | RX `--fps` 必须与发送端一致 |
| MPP 解码失败 | `dpkg -l \| grep mpp`；检查 extradata 日志 |
| 无音频 | `aplay -l`；调整 ALSA 设备 |

### hugepage（AF_XDP 路径）

```bash
# 查看
cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 释放 cache 后申请（需先停 MTL 进程）
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

单板建议 **256–512 页**（512MB–1GB）。

## 相关文档

- [config/README.md](config/README.md) — 配置字段速查
- [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) — 实现与验证阶段
- [doc/kernel_socket_vs_af_xdp.md](../../doc/kernel_socket_vs_af_xdp.md) — backend 选型
