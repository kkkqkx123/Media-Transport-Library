# mp4_dist — MP4 over MTL ST2110 (RK3588)

将 MP4 文件（H.264 + AAC）通过 **MTL ST2110-22（视频）/ ST2110-30（音频）** 实时发送；接收端 **MPP 硬解码 + DRM 显示 + ALSA 播放**，支持单板回环与 PC/RK 双机分离部署。

测试片源固定为仓库内 **`InTheGrey.mp4`**（1920×1080 @ 23.976fps），配置中路径为 `../InTheGrey.mp4`（相对本目录）。

---

## 应用架构

```
TX (mp4_dist_tx)                     网络 UDP 20000/20002              RX (mp4_dist_rx)
─────────────────                    ─────────────────────              ─────────────────
MP4 → FFmpeg demux                   ST2110-22 视频码流                 ST22 RX → bridge
  → H.264 Annex-B 拼接 extradata  ──►  ST2110-30 音频 PCM      ──►      → MPP 解码 → DRM
  → ST22 TX (CFR pacing)                                                → playout → A/V sync
  → ST30 TX                                                             ST30 RX → ALSA
```

| 组件 | 说明 |
|------|------|
| `mp4_dist_tx` | 发送端：FFmpeg 解复用 + MTL 发包（**x86 / ARM 均可编译**） |
| `mp4_dist_rx` | 接收端：MTL 收包 + MPP/DRM/ALSA（**需 RK3588 + MPP**） |
| `tests/mp4_dist_both.sh` | 单板回环：先 RX、2 秒后 TX（两个独立进程） |

---

## 目录结构

```
app/rk3588_mp4_dist/
├── mp4_dist_tx / mp4_dist_rx   # 可执行文件（build/ 或 build-tx/）
├── config/                     # JSON 配置（已纳入 git）
│   ├── tx_dual_host.json       # 双机：PC 发流
│   ├── rx_dual_host.json       # 双机：RK 收流
│   ├── tx_mp4_dist.json        # 单板：RK TX
│   └── rx_mp4_dist.json        # 单板：RK RX
├── tx/  rx/  shared/            # 业务模块
├── tests/                      # 验证脚本
├── meson.build
└── meson_options.txt           # build_tx / build_rx 开关
```

---

## 依赖

### 公共（TX + RX）

| 依赖 | 说明 |
|------|------|
| MTL ≥ 26.01 | 先编译安装主工程：`./build.sh release` |
| FFmpeg | libavformat / libavcodec / libavutil |
| json-c | 配置解析 |
| libbpf | MTL/DPDK EAL 运行时（PC TX 也需要） |

```bash
# Ubuntu/Debian 示例
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libjson-c-dev libbpf0
```

### 仅 RX（RK3588）

| 依赖 | 说明 |
|------|------|
| Rockchip MPP | 硬解码 |
| libdrm | HDMI/DRM 输出 |
| ALSA | 音频播放 |
| MtlManager + hugepages | 使用 `native_af_xdp:` 时必需 |

---

## 编译构建

本工程为 **独立 meson 项目**，需先安装 MTL 主库。

### RK3588 — TX + RX

```bash
cd app/rk3588_mp4_dist
./tests/setup_meson_options.sh    # 若缺少 meson_options.txt
meson setup build -Dbuild_tx=true -Dbuild_rx=true
ninja -C build

# 产物
ls build/mp4_dist_tx build/mp4_dist_rx
```

一键验证：`./tests/build_verify.sh full`

### x86 PC — 仅 TX

```bash
cd app/rk3588_mp4_dist
meson setup build-tx -Dbuild_tx=true -Dbuild_rx=false
ninja -C build-tx

# 产物
ls build-tx/mp4_dist_tx
```

一键验证：`./tests/build_verify.sh tx-only`

### 常见编译问题

| 报错 | 处理 |
|------|------|
| `Unknown option: build_tx` | 运行 `./tests/setup_meson_options.sh` 或确认存在 `meson_options.txt` |
| `build_rx requires Rockchip MPP` | PC 上加 `-Dbuild_rx=false` |
| `libbpf.so.0` 找不到 | `sudo apt install libbpf0` |

---

## 配置文件

所有配置位于 `config/`，视频文件统一为 **`../InTheGrey.mp4`**。

| 文件 | 场景 | 运行主机 |
|------|------|----------|
| `tx_dual_host.json` | 双机发送 | PC (192.168.80.213) |
| `rx_dual_host.json` | 双机接收 | RK3588 (192.168.80.212) |
| `tx_mp4_dist.json` | 单板发送 | RK3588 |
| `rx_mp4_dist.json` | 单板接收 | RK3588 |

默认 UDP：**视频 20000**，**音频 20002**（未在 JSON 中写出时使用程序默认值）。

### 双机配置摘要

**PC — `config/tx_dual_host.json`**

```json
{
  "tx_port": "kernel:enp1s0",
  "tx_lip": "192.168.80.213",
  "tx_dip": "192.168.80.212",
  "mp4_file": "../InTheGrey.mp4",
  "fps": 23.976,
  "loop": false,
  "mtl_lcores_tx": "1,2,3",
  "ptp_mode": "external"
}
```

**RK3588 — `config/rx_dual_host.json`**

```json
{
  "rx_port": "native_af_xdp:eth0",
  "rx_lip": "192.168.80.212",
  "rx_sip": "192.168.80.213",
  "fps": 23.976,
  "drm_mode": "auto",
  "mtl_lcores_rx": "1,2,3,4,5,6,7",
  "ptp_mode": "external"
}
```

> 网卡名请按 `ip link` 修改（PC 常见 `enp1s0`，RK 常见 `eth0` / `enP3p49s0`）。  
> **RX 的 `--fps` 必须与 TX 一致**（23.976）。

---

## PTP 时间同步（P5）

双机/多机场景需 **PC 与 RK 共享同一 PTP 时间域**，mp4_dist 默认使用 **外部 PTP**（linuxptp + `ptp_get_time_fn` → `CLOCK_TAI`），不再依赖 MTL 内置 HW PTP。

### 前置条件

1. **linuxptp ≥ 2.0**（单播需 2.0+；1.9.x 不支持 `unicast_listen`）
2. 内核 TAI offset：`sudo ../../tools/set_tai_offset/build/set_tai_offset`
3. 关闭 NTP：`sudo systemctl stop chrony systemd-timesyncd`

### 启动 PTP（双机单播）

```bash
# 终端 1 — PC (Master, 192.168.80.213)
cd app/rk3588_mp4_dist
chmod +x scripts/start_ptp_dual_host.sh
./scripts/start_ptp_dual_host.sh master enp1s0

# 终端 2 — RK (Slave)
./scripts/start_ptp_dual_host.sh slave eth0 192.168.80.213
```

配置文件：`config/ptp/ptp4l-master-unicast.conf`、`config/ptp/ptp4l-slave-unicast.conf`。

### 启动 mp4_dist（PTP 就绪后）

```bash
# RK 先 RX
./build/mp4_dist_rx 120 --config config/rx_dual_host.json

# PC 后 TX (+2s)
./build-tx/mp4_dist_tx 120 --config config/tx_dual_host.json
```

启动日志应出现：

```
MTL: PTP mode=external (linuxptp → CLOCK_TAI via ptp_get_time_fn)
PTP[kernel:enp1s0]: tai_clock=ok kernel_tai_offset=37 ...
av_sync: ... clock=ptp ...
```

### 配置项

| JSON / CLI | 说明 |
|------------|------|
| `ptp_mode`: `"external"` | 默认；linuxptp 同步后的 CLOCK_TAI |
| `ptp_mode`: `"builtin"` | MTL 内置 PTP（需 NIC HW timesync） |
| `ptp_mode`: `"monotonic"` | 无 PTP；RX 用 monotonic 相对锚定 |
| `ptp_utc_offset` | GM 为 UTC 域时从 TAI 减去的秒数（通常 0） |
| `--ptp-mode` / `--ptp-utc-offset` | CLI 覆盖 |

### 代码路径

- `shared/ptp_time.c` — MTL `ptp_get_time_fn` 读 TAI
- `rx/media_clk.c` — ST22 MEDIA_CLK 首帧锚定到 `mtl_ptp_read_time()`
- `rx/av_sync.c` — playout 调度与视频 PTS 同域（PTP）
- `rx/mtl_rx_audio.c` — ST30 使用 `timestamp_first_pkt` / `st10_get_tai()`
- `tx/mtl_tx_video.c` / `tx/mtl_tx_audio.c` — P7：`USER_TIMESTAMP` + TAI 时间轴（`shared/tx_tai_ts.c`）

---

## 运行说明

命令格式：`mp4_dist_tx|rx [秒数] [--config FILE] [CLI 覆盖...]`  
**不再有** `tx`/`rx`/`both` 子命令。

### 场景 A：双机（PC 发 + RK 收）

**先 RX，等 2 秒，再 TX。**

```bash
# RK3588 — 接收
cd app/rk3588_mp4_dist
./build/mp4_dist_rx 120 --config config/rx_dual_host.json

# PC — 发送（另开终端）
cd app/rk3588_mp4_dist
./build-tx/mp4_dist_tx 120 --config config/tx_dual_host.json
```

可选 trace（AU 对齐分析）：

```bash
export MP4_DIST_AU_TRACE=/tmp/dual    # 两端分别设置，路径前缀一致
export MP4_DIST_DUMP_RX=/tmp/rx_capture.h264   # 仅 RX
export MP4_DIST_DUMP_RX_IDX=/tmp/rx_capture.idx
```

### 场景 B：单板回环（RK3588 双网口）

需 **MtlManager** 运行中，且配置 hugepages（AF_XDP）。

```bash
cd app/rk3588_mp4_dist

# 方式 1：脚本（推荐）
./tests/mp4_dist_both.sh 120 \
  --config config/tx_mp4_dist.json \
  --fps 23.976

# 方式 2：手动两进程
./build/mp4_dist_rx 120 --config config/rx_mp4_dist.json &
sleep 2
./build/mp4_dist_tx 120 --config config/tx_mp4_dist.json
```

单板 TX 配置使用 `config/tx_mp4_dist.json`（RK 网口 `native_af_xdp:enP3p49s0`）。

### 场景 C：CLI 覆盖示例

```bash
# TX：指定 MP4 + 禁用循环
./build/mp4_dist_tx 60 --config config/tx_dual_host.json --no-loop

# RX：显式 fps（必须与 TX 一致）
./build/mp4_dist_rx 60 --config config/rx_dual_host.json --fps 23.976

# PC 4 核：限制 MTL worker（勿用 RK 的 1-7）
./build-tx/mp4_dist_tx 120 --config config/tx_dual_host.json --mtl-lcores-tx 1,2,3
```

---

## 验证脚本

| 脚本 | 用途 |
|------|------|
| `tests/build_verify.sh full` | 编译 TX+RX |
| `tests/build_verify.sh tx-only` | 仅编译 TX |
| `tests/verify_phase.sh P4 120 ../InTheGrey.mp4` | P4 阶段回归 |
| `tests/run_single_board_plan_a.sh 120 ../InTheGrey.mp4` | trace + dump + 离线分析 |
| `tests/check_pts_idx.py` | PTS 索引检查 |
| `tests/check_playout_log.py` | playout 日志检查 |
| `tests/check_drm_log.py` | DRM 刷新率检查 |

---

## CPU / lcore 说明（RK3588）

| CPU | 类型 | 典型用途 |
|-----|------|----------|
| 0–3 | A55 | bridge、audio |
| 4–7 | A76 | decode、render |

- **`mtl_lcores_*`**：MTL/DPDK worker 列表，**不含 CPU 0**（0 为 EAL main）
- **PC 4 核**：用 `1,2,3`；程序会自动裁剪超出本机 CPU 数的配置
- **单板 both + MtlManager**：RX/TX 的 `mtl_lcores` 应使用同一列表

---

## 故障排查

| 现象 | 排查 |
|------|------|
| MTL init 失败 | AF_XDP：检查 MtlManager、hugepages；PC 可改 `kernel:<ifname>` |
| `libbpf.so.0` 缺失 | `sudo apt install libbpf0` |
| `EAL: lcore N unavailable` | 减小 `mtl_lcores_*`（PC 用 `1,2,3`） |
| RX 无画面 | 确认 `rx_sip` = TX 的 `tx_lip`；`tcpdump -i eth0 udp port 20000` |
| fps 不匹配 | RX 必须 `--fps 23.976`，与 TX/MP4 一致 |
| MPP 解码失败 | `dpkg -l \| grep mpp`；检查 extradata 日志 |
| 无音频 | `aplay -l`；调整 ALSA 设备 |
| hugepage 不足 | 见下方；或 RX 暂改 `kernel:eth0` 调试 |

### hugepage（AF_XDP 路径）

```bash
# 查看
cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 释放 cache 后申请（需先停 MTL 进程）
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

单板 mp4_dist 建议 **256–512 页**（512MB–1GB），不必一次配 2048 页。

---

## 相关文档

- [config/README.md](config/README.md) — 配置字段速查
- [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) — 实现与验证阶段
- [doc/kernel_socket_vs_af_xdp.md](../../doc/kernel_socket_vs_af_xdp.md) — backend 选型
