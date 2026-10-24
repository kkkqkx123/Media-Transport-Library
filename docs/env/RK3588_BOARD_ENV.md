# RK3588 开发板环境实测记录（board = 192.168.80.202）

采集方式：`ssh board`（root），全部为板上实测结果，非推断。采集时间：**2026-10-24**。
配套阅读：`docs/analysis/AF_XDP_EBPF.md`（AF_XDP 集成方案）、`docs/app/rk3588_end2end.md`、`docs/app/rk3588_mp4_dist.md`。

---

## 1. 一句话结论

**内核层面支持 AF_XDP，但这块板当前跑不了 MTL 的 native AF_XDP。** AF_XDP socket 全流程（`socket(PF_XDP)` → UMEM 注册含 `XDP_UMEM_UNALIGNED_CHUNK_FLAG` → RX/FILL/COMPLETION ring → `bind`）在 eth0/eth1 上实测全部成功；阻塞点全部在 hugepage 缺失、用户态依赖缺失（libxdp/xdp-tools/clang）和网卡驱动只支持 generic XDP 这三处。

---

## 2. 板级硬件与系统

| 项 | 实测值 |
|---|---|
| 型号（device-tree model） | `ztl, EN588` |
| compatible | `rockchip,rk3588-evb7-v11`, `rockchip,rk3588` |
| 内核 | `6.1.118 #206 SMP Tue Jul 7 18:53:46 CST 2026 aarch64` |
| 发行版 | Ubuntu 22.04.5 LTS (Jammy)，aarch64 |
| 主机名 | `RK3588` |
| CPU | 8 核 big.LITTLE：cpu0-3 = Cortex-A55（`0xd05`，scaling_max 1800 MHz），cpu4-7 = Cortex-A76（`0xd0b`，2256 MHz） |
| cluster_id（sysfs 读数） | cpu0→0，cpu4→1，cpu7→2 |
| cpufreq governor | `performance` |
| 内存 | MemTotal 8104952 kB（≈7.9 GiB），MemAvailable ≈7.3 GiB，**SwapTotal 0** |
| 根盘 | eMMC `/dev/root` 57G，已用 7.8G（`mmcblk0` 58.2G，6+ 分区） |
| `/proc/cmdline` | `storagemedia=emmc androidboot.storagemedia=emmc androidboot.mode=normal androidboot.verifiedbootstate=orange androidboot.serialno=... rw rootwait earlycon=uart8250,mmio32,0xfeb50000 console=ttyFIQ0 irqchip.gicv3_pseudo_nmi=0 root=PARTUUID=614e0000-0000 rcupdate.rcu_expedited=1 rcu_nocbs=all androidboot.fwver=ddr-v1.19-...,uboot-07/07/2026` |
| 时区 | CST (UTC+8)，无 RTC |
| SELinux / IMA | 未启用（无相关 cmdline 项） |
| `unprivileged_bpf_disabled` | `2`（锁定；root 仍可加载 BPF） |
| isolated CPU | 无（`possible = 0-7`，未做 isolcpus） |

### 2.1 内存相关（对 MTL 是硬门槛）

| 项 | 实测值 | 影响 |
|---|---|---|
| `CONFIG_HUGETLBFS` | **`# is not set`** | `CONFIG_HUGETLB_PAGE` 因此根本不存在 |
| `/proc/meminfo` 的 `Huge*` 行 | **0 行** | 无任何 hugepage |
| `/sys/kernel/mm/hugepages/` | **不存在**（该目录只有 `swap`） | `nr_hugepages` 无处可写 |
| `/dev/hugepages`、`/mnt/huge*` | **不存在** | DPDK EAL 无 mount 点 |
| `vm.nr_hugepages` | 设置无效（内核不支持） | `sysctl -w vm.nr_hugepages=2048` 这条路走不通 |
| THP | `# CONFIG_TRANSPARENT_HUGEPAGE is not set`，`/sys/kernel/mm/transparent_hugepage/` 不存在 | 无替代大页 |
| CMA | `CONFIG_CMA=y`、`CONFIG_CMA_SIZE_MBYTES=16` | **DMA-CMA ≠ hugetlb**，不能拿来当 MTL 内存池 |
| `CONFIG_MODULES` | `=y`（可加载模块） | 但 `/lib/modules/6.1.118/build` 不存在，板上无法编外部模块 |

> **结论**：MTL 整套内存体系基于 DPDK hugepage，AF_XDP 后端又是把 mempool 的 hugepage 内存直接注册成 UMEM（`docs/analysis/AF_XDP_EBPF.md` §3），所以本板在**重编内核打开 `CONFIG_HUGETLBFS`** 之前，`MtlManager` 与任何 MTL 应用（不论 DPDK 还是 AF_XDP 后端）都起不来。这是比 AF_XDP 更靠前的阻塞项。

---

## 3. 网口实况（与文档假设不一致，重点）

| 项 | eth0 | eth1 |
|---|---|---|
| udev altname | **`enP2p33s0`** | **`enP3p49s0`** |
| 驱动 | `yt6801` 1.0.30（**builtin**，`drivers/ztl/yt6801`） | `yt6801` 1.0.30 |
| PCI 地址 | `0002:21:00.0` | `0003:31:00.0` |
| 固件 | `S.D.U: 52.0.10` | 同 |
| MAC | `c6:34:f6:f0:3a:ad` | `d6:34:f6:f0:4a:ad` |
| 状态 | **UP**，`192.168.80.202/24`（SSH 走这个口） | **NO-CARRIER**，没插网线 |
| 协商速率 | **100 Mb/s** | Unknown |
| 队列（`ethtool -l` current） | RX 4 / **TX 1** / combined 4 | RX 4 / TX 1 |
| sysfs 队列目录 | `rx-0..rx-3`、**只有 `tx-0`** | 同 |
| `tx_maxrate` 属性 | 存在于 `tx-0/`，**写入返回 EOPNOTSUPP** | 同 |
| PTP 硬件时钟 | **none**，`Hardware Transmit Timestamp Modes: none`，仅 `software-receive`/`software-system-clock` | 同 |
| `/dev/ptp*` | **不存在** | — |
| 其它接口 | `can0/can1` DOWN、`docker0` DOWN(172.17.0.1/16)、`wlan0` DOWN | — |

要点：

- 网卡不是 `docs/analysis/AF_XDP_EBPF.md` §5/§6 假设的 **r8168/RTL8125**，而是 **Motorcomm YT6801**（PCIe，厂商 out-of-tree/builtin 驱动）。
- 文档里配置的 `tx_port = "native_af_xdp:enP3p49s0"` 现在指向的是**没插网线的 eth1**；板上唯一通的是 eth0（`enP2p33s0`），而它只协商到 **100 Mb/s** —— 单路 1080p ST2110 约需 1.2 Gbps 以上，当前物理链路连一路都跑不满。改端口名之前先解决接线与速率。
- `ethtool -T eth0` 无 HW timesync、无 `/dev/ptp*`、板上未装 `ptp4l`/`phc2sys` → `rk3588_mp4_dist` 依赖的外部 PTP（CLOCK_TAI / linuxptp）方案在本板**当前不可用**，只能用软件时间戳或系统时钟。
- "单队列"约束只在 **TX 侧**成立（RX 有 4 队列），`e2e_app.c` 里 `rx_queues_cnt=1` 的注释依据（r8168）与实情不符，但结论仍然安全。

---

## 4. AF_XDP / XDP / BPF 实测结果

### 4.1 内核配置（`zcat /proc/config.gz`）

| 配置项 | 值 | 对 MTL 的意义 |
|---|---|---|
| `CONFIG_XDP_SOCKETS` | **`=y`** | AF_XDP socket 层存在（`docs/analysis/AF_XDP_EBPF.md` §6 前提 1 满足） |
| `CONFIG_BPF_SYSCALL` | **`=y`** | eBPF 程序/map 可加载 |
| `CONFIG_BPF` | `=y` | |
| `CONFIG_BPF_STREAM_PARSER` / `CONFIG_CGROUP_BPF` / `CONFIG_BPF_EVENTS` | 全部 `=y` | |
| `CONFIG_HAVE_EBPF_JIT` | `=y`（仅能力声明） | |
| **`CONFIG_BPF_JIT`** | **`# is not set`** | **eBPF 只能解释器执行**；`/proc/sys/net/core/bpf_jit_enable` 连文件都不存在。µs 级 pacing 下 dispatcher + `mtl_dp_filter` 每包过解释器，是性能硬伤 |
| `CONFIG_XDP_SOCKETS_DIAG` | `# is not set` | 无 xsk 诊断（影响观测，不影响功能） |
| `CONFIG_DEBUG_INFO_BTF` | **不存在**，`/sys/kernel/btf/vmlinux` 缺失 | CO-RE 程序无法重定位；`bpftool btf dump` 这条路不可用 |
| `CONFIG_KPROBES` | `# is not set` | `tools/ebpf/` 的 `et`(fentry) / `lcore_monitor` 在本板**完全不可用** |
| `/sys/fs/bpf` | **已挂载**（type `bpf`, mode 700） | bpffs 可用 |
| `unprivileged_bpf_disabled` | `2`（锁定） | 非 root 不能加载 BPF；root 可 |

### 4.2 AF_XDP socket 能力实测（关键证据）

用不依赖 libbpf/libxdp 的 raw-syscall 探针 `probes/xsk_probe.c`，走 `mt_af_xdp.c` 需要的完整 socket 建立流程，**eth0 与 eth1 结果一致，全部成功**：

| 步骤 | 调用 | 结果 |
|---|---|---|
| 1 | `socket(PF_XDP, SOCK_RAW, 0)` | `fd=3`，errno=0 ✅ |
| 2 | `setsockopt(XDP_UMEM_REG)`，flags = **`XDP_UMEM_UNALIGNED_CHUNK_FLAG`** | `ret=0` ✅ —— 这正是 `mt_af_xdp.c` 顶部 `#error` 卡的那个要求，6.1 内核满足 |
| 3 | `setsockopt(XDP_RX_RING, 64)` | `ret=0` ✅ |
| 4 | `setsockopt(XDP_UMEM_FILL_RING, 64)` | `ret=0` ✅ |
| 5 | `setsockopt(XDP_UMEM_COMPLETION_RING, 64)` | `ret=0` ✅ |
| 6 | `getsockopt(XDP_MMAP_OFFSETS)` | `ret=0`，optlen=128 ✅ |
| 7 | `bind(<if>, queue 0)` | **`ret=0`** ✅（eth0、eth1 都成功） |

> 内核侧 AF_XDP 能力判定：**支持，且非对齐 UMEM 可用**。

### 4.3 XDP 挂载模式实测（决定后端形态）

用 clang 编的最小非 CO-RE XDP 程序 `probes/xdp_probe.c`（`SEC("xdp")` 返回 `XDP_PASS`/`XDP_DROP`，无 BTF 依赖）在 eth1（无载波，零流量风险）上测：

| 挂载模式 | 命令 | 结果 |
|---|---|---|
| **native / driver** | `ip -force link set dev eth1 xdpdrv obj /tmp/xdp_probe.o sec xdp` | **失败**：`Error: Underlying driver does not support XDP in native mode.` |
| **generic / SKB** | `ip -force link set dev eth1 xdpgeneric obj /tmp/xdp_probe.o sec xdp` | **成功**，`ip -d link show eth1` 出现 `prog/xdp id 45 tag 3fed666785a57e53` |
| 清理 | `ip link set dev eth1 {xdpgeneric,xdp} off` | 已确认无残留（测毕板上 `/tmp` 探针已删除，未对 eth0 做任何挂载） |

推论（对应 `AF_XDP_EBPF.md` §3、§6 风险表第 1 行）：

- MtlManager 侧 `load_xdp()` 先试 `XDP_MODE_NATIVE` 会失败，**自动退 `XDP_MODE_SKB`** → 可用但每包多一次 SKB 拷贝路径开销。
- 应用侧 `xsk_socket__create` 强制 `XDP_ZEROCOPY` 会失败 → **退 copy 模式（有 memcpy）**。故本板上 MTL AF_XDP 只能是「generic XDP + copy UMEM」形态，零拷贝不可得。
- `yt6801` 为 builtin 驱动（`drivers/ztl/yt6801`），无 `ndo_bpf`；要 native/ZC 必须换网卡或给驱动补 `ndo_bpf`。

### 4.4 队列级限速（`rl_type`）实测

| 检查 | 结果 |
|---|---|
| MTL 探测路径 `/sys/class/net/<if>/queues/tx-1/tx_maxrate` | **不存在**（板上只有 `tx-0`）→ `mt_sysfs_write_uint32()` 失败，`XDP_F_RATE_LIMIT` 不置位，`rl_type` 不会设成 `MT_RL_TYPE_XDP_QUEUE_SYSFS`，**干净回落软件 pacing** |
| 手工写 `eth1/queues/tx-0/tx_maxrate` | `write FAILED`（返回码 1） |
| 手工写 `eth0/queues/tx-0/tx_maxrate` | `printf: 写入错误：不支持的操作` = **EOPNOTSUPP** |
| 属性可见性 | 只有 `tx-0` 有该文件，`tx-1..tx-3` 无 |

> 与 `AF_XDP_EBPF.md` §3 的预判一致：本板**没有队列级硬件限速**，只能用 TSC 软件 pacing（即两个 rk3588 应用的既定选择）。

### 4.5 板上用户态依赖现状

| 组件 | 现状 | 备注 |
|---|---|---|
| `libxdp` / `xsk.h` / `xdp-loader` | **全盘找不到**（`find / -name 'libxdp*' -o -name xdp-loader` 空） | apt 无 `libxdp-dev`、无 `xdp-tools` 候选 |
| `libbpf` | 只有运行库 `libbpf0 1:0.5.0-1ubuntu22.04.1`；**无 `libbpf-dev`、无头文件、无 pkg-config 条目** | apt 候选最高 `1:0.5.0`（jammy），比 `versions.env` 需要的 libbpf 版本老 |
| `clang` / `llvm-strip` | **都没有** | 板上无法编 `mtl.xdp.o`（manager 强制要求） |
| `bpftool` | 没有（`linux-tools` 未装） | |
| `/usr/include/linux/if_xdp.h` | 存在 | 所以能编 raw-syscall 程序（本记录的证据即由此而来） |
| `iproute2` | `5.15.0-1ubuntu2.2`（支持 `ip link set ... xdp[x] obj`） | 挂载测试可用 |
| Python | 3.10.12，**`socket.AF_XDP` 常量不存在**（故不能用 python 直接测 xsk） | |
| MTL 安装 | `/usr/local/lib/libmtl*`、`/usr/local/bin/MtlManager`、`/usr/local/lib/bpf/` **全部不存在** | 板上目前没装任何 MTL |

---

## 5. 阻塞项排序（按处理优先级）

| # | 阻塞 | 严重度 | 处置 |
|---|---|---|---|
| 1 | **无 hugepage**（`CONFIG_HUGETLBFS` 未开） | 致命，且不止挡住 AF_XDP——挡住 MTL 全部后端 | 重编内核打开 `CONFIG_HUGETLBFS`（连带 `HUGETLB_PAGE`），并准备 hugetlbfs mount 点与 cmdline `hugepages=` |
| 2 | **无 libxdp / libbpf-dev / clang** | 致命 | 交叉编 `script/build_ebpf_xdp.sh`（xdp-tools 1.6.3 + libbpf，见 `versions.env`）并部署到板上 sysroot；manager 的 `mtl.xdp.o` 在有 clang 的一侧编好后拷贝 |
| 3 | **yt6801 无 native XDP**（只能 generic + copy） | 性能致命，功能不致命 | 接受 copy 模式做功能验证；要性能需换支持 native/ZC 的网卡，或给驱动补 `ndo_bpf` |
| 4 | **`CONFIG_BPF_JIT` 未开** | 性能 | 重编内核打开 `BPF_JIT`（与 #1 同一批内核改动最省事） |
| 5 | **无 BTF / 无 KPROBES** | 观测能力缺失 | 需要 `tools/ebpf` 类 CO-RE/fentry 工具时打开 `DEBUG_INFO_BTF` + `KPROBES` |
| 6 | eth1 没插线、eth0 只 100 Mb/s | 物理层，最先决 | 先接线并确认链路能到 1G/2.5G，否则任何 ST2110 流都跑不动 |
| 7 | 无 HW timesync / 无 `/dev/ptp*` / 无 linuxptp | 影响 mp4_dist 的 PTP 方案 | 装 linuxptp 走软件时间戳，或改用系统时钟 |

---

## 6. 与 `docs/analysis/AF_XDP_EBPF.md` 的偏差（待回改文档）

| 文档表述 | 板上实情 |
|---|---|
| §5/§6 网卡为 **r8168 / RTL8125** | 实为 **Motorcomm YT6801**（builtin `drivers/ztl/yt6801`） |
| `tx_port = native_af_xdp:enP3p49s0`（可发的口） | `enP3p49s0` = **eth1，NO-CARRIER 未插线**；活动口是 `eth0`/`enP2p33s0` |
| §3 限速 sysfs 名 `tx_max_rate` | 代码实际是 **`tx_maxrate`**（`mt_af_xdp.c:96/284`），文档笔误 |
| §3 未提探测队列号 | 探测**写死 tx-1**（`mt_af_xdp.c:284`），单 TX 队列网卡上必然探测失败 |
| §6 步骤 1 「`grep CONFIG_XDP_SOCKETS /proc/config.gz`」 | 板上该项 `=y`，**已满足**；但同一步骤要求的 `CONFIG_BPF_JIT` **未满足**，文档未把它列为硬性前提 |
| §5 「r8168 单队列约束」 | TX 侧单队列成立，RX 有 4 队列 |
| §6 风险表「用 `bpftool btf dump /sys/kernel/btf/vmlinux` 确认 BTF」 | 本板**无 BTF 也无 bpftool**，该验证手段不可用；MTL 自带 `mtl.xdp.c` 是 plain XDP + hash map，理论上不依赖 BTF，但因板上无 libxdp **未能实测加载** |

---

## 7. RK 加速与多媒体栈（两个 rk3588 应用的依赖现状）

| 依赖 | 用途 | 板上现状 | 判定 |
|---|---|---|---|
| **MPP** | `rk3588_end2end` 编解码、`mp4_dist` 解码 | `rockchip_mpp` pkg-config **1.3.9**，头文件 `/usr/include/rockchip/mpp_*.h` 齐备，`librockchip_mpp.so{,.0,.1}` 在 `/usr/lib/aarch64-linux-gnu/`，**`/dev/mpp_service` 存在**，示例二进制 `/usr/bin/mpi_enc_test`、`mpi_dec_test`、`mpi_dec_mt_test` 等已在 | ✅ 可直接编译运行 |
| **RGA** | end2end 的颜色/格式转换 | `librga` **2.1.0**，头文件 `/usr/include/rga/`（`im2d_*.h`、`GrallocOps.h`、`drmrga.h`），**`/dev/rga` 存在** | ✅ |
| **DRM/KMS** | `drm_renderer` 渲染、`rx_compositor` 合成 | `/dev/dri/card0`、`card1`、`renderD128`、`renderD129`；`libdrm` **2.4.114** + pkg-config；`gbm`/`egl`/`glesv2`/`wayland-egl` 均在 | ✅ 节点齐全（**connector/plane/模式未实测**，需上屏验证） |
| **V4L2 采集** | end2end 的采集源 | **只有 `/dev/video40`**，`v4l2-ctl` 显示 driver/card = **`rk_hdmirx`**（bus `fdee0000.hdmirx-controller`，capabilities: Video Capture Multiplanar + Streaming）；`/dev/v4l/by-path` 存在，`libv4l2` 1.22.1 + `libv4l2.h` | ⚠️ 采集源是 **HDMI-IN，不是摄像头**；无 `/dev/media*`（无 media controller）、无 ISP 节点（虽装了 `camera-engine-rkaiq` 6.30.4）。multiplanar 路径必须走通 |
| **ALSA** | `mp4_dist` 音频输出 | 声卡 4 张：`rockchip-hdmiin`(0)、**`rockchip-es8388`(1)**、`rockchip-hdmi0`(2)、`rockchip-hdmi1`(3)，`aplay -l` 可见；但 **`alsa` pkg-config 缺失、`/usr/include/alsa/asoundlib.h` 不存在** | ❌ 运行时可用，**编译期缺 dev 包**：需 `apt install libasound2-dev` |
| **GStreamer / FFmpeg** | 备选管线 | `gst-launch-1.0` 1.20.3 + `rockchipmpp` 插件（`mpph264enc`/`mpph265enc`/`mppvideodec`/`mppjpegenc`…共 8 个 RK 元件）；`ffmpeg` 已装（自建串 `7d0f880f0a`）；`v4l2-ctl` 在 | ✅ |
| **OpenCV** | 可选 | `opencv4` pkg-config 存在 | ✅ |
| **linuxptp** | `mp4_dist` 的 PTP 授时 | `ptp4l`/`phc2sys` **未安装**，无 `/dev/ptp*`，网卡无 HW timesync | ❌（见 §3） |

---

## 8. 构建工具链与 MTL 本体状态

| 工具 | 版本 / 状态 |
|---|---|
| `meson` | **0.61.2** ✅（MTL 各子项目可用） |
| `ninja` | **1.10.1** ✅ |
| `gcc` | 11.4.0（Ubuntu 11.4.0-1ubuntu1~22.04.3）✅ |
| `pkg-config` | 0.29.2 ✅ |
| `make` / `cmake` / `autoconf` / `git` | 4.3 / 3.22.1 / 2.71 / 2.34.1 ✅ |
| `libtool` | **缺失** ❌（xdp-tools 交叉/本机编译需要） |
| `clang` / `llvm-strip` | **缺失** ❌（`mtl.xdp.o` 无法在板上生成） |
| `bpftool` / `linux-tools` | **缺失** ❌ |
| Python | 3.10.12（`socket.AF_XDP` 不存在，不能用 python 测 xsk）|
| 内核构建目录 | **`/lib/modules/6.1.118/build` 不存在** ❌ → 板上无法编外部内核模块（但 `/proc/config.gz` 可用，配置可查） |
| **DPDK** | **完全没有**（`ldconfig -p` 无任何 `librte*`/`libdpdk`，无 dpdk 相关包）❌ |
| **MTL** | **没有**（无 `libmtl*`、无 `MtlManager`、无 `RxTxApp`）❌ |
| `docker` | 29.9.2（有 `docker0` 网桥，DOWN） |
| 网络 / apt | 外网可通（ping 223.5.5.5 17.7ms），`apt-get update` **exit 0**（源 `mirrors.aliyun.com/ubuntu-ports` jammy）→ 缺的 dev 包基本可 apt 补，但 **libxdp/xdp-tools 无候选**（见 §4.5） |

---

## 9. 复测方法

探针源码随本目录存档：`docs/env/probes/xsk_probe.c`（AF_XDP socket 全流程）、`docs/env/probes/xdp_probe.c`（最小非 CO-RE XDP 挂载探针）。

```bash
# 1) AF_XDP socket 能力（不需要 libbpf/libxdp，只要 linux/if_xdp.h）
scp docs/env/probes/xsk_probe.c board:/tmp/
ssh board 'gcc -O2 -Wall -o /tmp/xsk_probe /tmp/xsk_probe.c && /tmp/xsk_probe eth0 && /tmp/xsk_probe eth1'

# 2) XDP 挂载模式：native 与 generic 分别试（用无载波的口，零流量风险）
clang -O2 -target bpf -c docs/env/probes/xdp_probe.c -o /tmp/xdp_probe.o   # 本机(x86)编即可，BPF 字节码跨架构
scp /tmp/xdp_probe.o board:/tmp/
ssh board 'ip -force link set dev eth1 xdpdrv obj /tmp/xdp_probe.o sec xdp'    # native
ssh board 'ip -force link set dev eth1 xdpgeneric obj /tmp/xdp_probe.o sec xdp' # generic/SKB
ssh board 'ip -d link show eth1 | grep -i xdp'
ssh board 'ip link set dev eth1 xdp off; ip link set dev eth1 xdpgeneric off; rm -f /tmp/xsk_probe* /tmp/xdp_probe.o'

# 3) 内核/驱动/队列/限速能力速览
ssh board 'zcat /proc/config.gz | grep -E "XDP_SOCKETS|BPF_SYSCALL|BPF_JIT|HUGETLBFS"'
ssh board 'ethtool -i eth0; ethtool -l eth0; ethtool -T eth0; ls /sys/class/net/eth0/queues/'
```

注意事项：

- 挂载测试请在**非 SSH 承载口**上做（本板 eth0 就是登录口）；程序只返回 `XDP_PASS` 时理论上透明，但无必要不动承载业务流量的口。
- BPF 对象可在 x86 上用 clang `-target bpf` 编好直接拷到 arm64 加载（BPF 字节码与端序在 x86_64/arm64 间一致），前提是不依赖 CO-RE/BTF。

---

## 10. 待办 / 未覆盖项

| 项 | 说明 |
|---|---|
| 内核重编 | 需打开 `CONFIG_HUGETLBFS`（→`HUGETLB_PAGE`）、`CONFIG_BPF_JIT`，建议同时开 `CONFIG_DEBUG_INFO_BTF`+`CONFIG_KPROBES`+`CONFIG_XDP_SOCKETS_DIAG`；板上无内核构建目录，须在 SDK 侧改 |
| eth1 接线与链路速率 | 确认能协商到 1G/2.5G，否则 ST2110 无意义（见 §3） |
| 换网卡 | 要 native XDP / zero-copy，需 `ndo_bpf` 支持的网卡（当前 yt6801 不支持） |
| `mtl.xdp.o` 加载 | 本板无 libxdp，**未实测**；装好 libxdp 后需验 plain-XDP + hash map 在无 BTF 内核上能否被 dispatcher 加载 |
| DRM 上屏 | connector/plane/`rx_compositor` 多窗口合成未实测 |
| HDMI-RX 采集 | `/dev/video40` 的实际分辨率/色彩格式、multiplanar 与 MPP 输入对接未实测 |
| ALSA 编译依赖 | 待装 `libasound2-dev` 后验证 `mp4_dist` 能否编过 |

