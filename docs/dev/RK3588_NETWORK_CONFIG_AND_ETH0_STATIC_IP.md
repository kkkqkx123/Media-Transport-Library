# RK3588 网络配置与 eth0 静态 IP 设置

本笔记整理 RK3588（6.1 内核）上两类“网络相关”定制，供后续移植/排障参考：

1. **内核网络设置**：为 MTL `native_af_xdp` 后端开启的内核 AF_XDP / eBPF 配置项，以及它们在内核源码中的位置。
2. **eth0 静态 IP**：因镜像无桌面端、内网无 DHCP，如何把固定 IP 直接编译进 rootfs。

对应补丁都在仓库 `patches/rk3588/`：

| 补丁 | 内容 |
|---|---|
| `0001-kernel-6.1-enable-af_xdp-ebpf-for-mtl.patch` | 内核 AF_XDP/eBPF 使能（包含两个提交：defconfig 使能 + `STMMAC_UIO` 6.1 编译修复） |
| `0002-rk3588-fs-overlay-eth0-static-ip.patch` | rootfs overlay：eth0 静态 IP `192.168.80.202/24`，网关 `192.168.80.254` |

---

## 1. 内核网络设置（AF_XDP / eBPF）

### 1.1 为什么需要

MTL 的 `native_af_xdp` 收发包后端依赖内核：

- `AF_XDP` 套接字（用户态零拷贝收发包）→ `CONFIG_XDP_SOCKETS`
- BPF 程序加载/校验、JIT → `CONFIG_BPF_SYSCALL` / `CONFIG_BPF_JIT`
- libbpf 读取内核类型信息 → `CONFIG_DEBUG_INFO_BTF`
- MTL 大页 buffer → `CONFIG_HUGETLBFS`

5.10 树（`rk3588_5.1_XDP`，5.10.226）原本就有这些配置；移植到 6.1 SDK（6.1.118）时由 `0001` 补丁重新落地。

### 1.2 补丁改了什么

- 新增 `kernel-6.1/arch/arm64/configs/ztl_defconfig`：以 `rockchip_linux_defconfig` 为基线 + ztl 板级 delta，集中开启网络/BPF 相关选项（见 1.3 表）。
- 新增 `kernel-6.1/arch/arm64/configs/ztl_rk3588.config` 片段：板级增量（`CONFIG_YT6801=y`、WiFi/CAN/USB 网卡等）。
- 修改 `device/rockchip/common/configs/Config.in.kernel`：
  - `RK_KERNEL_CFG` 默认值改为 `ztl_defconfig`；
  - rk3588 芯片家族的 `RK_KERNEL_CFG_FRAGMENTS` 默认 `ztl_rk3588.config`。
- `kernel-6.1/arch/arm64/boot/dts/rockchip/ztl-rk3588-cm.dts`：禁用 `i2c4`（板级调试口，与 5.1 XDP 树保持一致）。
- 同补丁第 2/2 提交：`ztl_defconfig` 中关闭 `CONFIG_STMMAC_UIO`——6.1 的 `stmmac_priv` 结构体布局已变（`rx_queues_cfg[]`/`tx_queues_cfg[]` + `dma_rx_size`/`dma_tx_size`），原 `stmmac_uio.c` 引用旧成员无法编译，且与 AF_XDP 数据路径冲突。

> 注意：`ztl_defconfig` 是全量配置（约 919 行），不仅是网络项；移植其他板时请整体评估，不要只挑网络行。

### 1.3 关键内核配置项 → 源码位置对照

| 配置项 | 作用 | 内核源码位置 |
|---|---|---|
| `CONFIG_XDP_SOCKETS` | AF_XDP 套接字，用户态零拷贝收发包 | `net/xdp/`（`xsk.c`、`xsk_buff_pool.c`、`xdp_umem.c`、`xskmap.c`、`xsk_queue.c`）；`net/core/sock.c`（AF_XDP 地址族）；`include/net/xdp_sock.h`。Kconfig：`net/xdp/Kconfig` |
| `CONFIG_XDP_SOCKETS_DIAG` | AF_XDP 诊断（`ss -xdp`） | `net/xdp/xsk_diag.c` |
| `CONFIG_BPF_SYSCALL` | BPF 系统调用 `bpf()` | `kernel/bpf/syscall.c` |
| `CONFIG_BPF_JIT` / `BPF_JIT_ALWAYS_ON` | BPF JIT 编译器（arm64） | `arch/arm64/net/bpf_jit_comp.c`、`kernel/bpf/core.c` |
| `CONFIG_BPF_STREAM_PARSER` | BPF stream parser | `net/core/stream_parser.c` |
| `CONFIG_CGROUP_BPF` | cgroup 级 BPF 挂载 | `kernel/bpf/cgroup.c` |
| `CONFIG_HUGETLBFS` / `CGROUP_HUGETLB` | 大页文件系统（MTL buffer） | `fs/hugetlbfs/`、`mm/hugetlb.c` |
| `CONFIG_DEBUG_INFO` / `DEBUG_INFO_BTF` | BTF（`/sys/kernel/btf/vmlinux`，libbpf 必需） | BTF 由 `pahole`（dwarves）构建时生成；源码 `kernel/bpf/btf.c`、`scripts/pahole-flags.py`。**构建机需装 `dwarves`/`pahole`**，且 `DEBUG_INFO_REDUCED` 必须关掉 |
| `CONFIG_YT6801` | 板载千兆网卡驱动（YT6801，出 `eth0`） | `drivers/net/ethernet/yt6801/`（`fuxi-gmac-*.c`） |

### 1.4 重要限制：YT6801 不支持原生 XDP / 零拷贝

实测 `drivers/net/ethernet/yt6801/fuxi-gmac-net.c` 中只有**被注释掉**的 XDP 残留：

```c
/* synchronize_rcu() needed for pending XDP buffers to drain */
//if (adapter->xdp_ring[0]) 20210709
```

没有实现 `ndo_xdp_xmit` / `NETDEV_XDP_ACT_NDO_XMIT` / `XDP_ZEROCOPY`。

**含义**：在该 6.1 树上，AF_XDP 在 YT6801（`eth0`）上只能以 **skb 模式（copy）** 运行，**不能零拷贝**。MTL `native_af_xdp` 后端仍可工作，但吞吐/延迟性能受限于拷贝；若需要 zc 性能，需要给 YT6801 驱动补全 XDP 钩子（新增工作量，不在本补丁范围）。

> 排障提示：若 MTL 报 “zero-copy not supported” 或自动退到 skb 模式，属预期，源于此驱动限制，不是配置漏开。

### 1.5 板端验证（6.1 真正跑起来后）

```bash
uname -a                                  # 应为 6.1.118，编译者 itc@itc
zcat /proc/config.gz | grep -E "XDP_SOCKETS=|BPF_JIT=|HUGETLBFS=|DEBUG_INFO_BTF="
ls /sys/kernel/btf/vmlinux                # 应存在
ip -br link show eth0                     # YT6801 出 eth0
```

> 若 `uname` 仍显示 5.10 且编译者不是 `itc@itc`，说明 update.img 没真正刷进 boot 分区（见 `RK3588_6.1_KERNEL_FULL_BUILD.md` 的烧录未生效排查），与配置无关。

### 1.6 单独重编内核 / 生成 boot.img

```bash
cd /data/kkkqkx/rk3588bsp
./build.sh kernel-6.1        # 仅重编内核并生成 boot.img（不触动 rootfs）
```

- `ztl_defconfig` 由 SDK 的 `Config.in.kernel` 默认选中；改内核配置后走：
  ```bash
  cd kernel-6.1 && make ARCH=arm64 ztl_defconfig && make ARCH=arm64 olddefconfig
  ```
- 产物：`output/firmware/boot.img`（软链 `output/update/Image/boot.img`）。

---

## 2. 设置 eth0 静态 IP（rootfs overlay）

### 2.1 动机与实测机制

- 现象：镜像无桌面端，且部署内网无 DHCP 服务器 → eth0 拿不到地址。
- 实测（从构建产物确认，非猜测）：`BR2_INIT_BUSYBOX`（busybox init）；`/etc/network/interfaces` 默认只有 `lo` + `source-directory interfaces.d`，**无 eth0 静态配置**；eth0 地址实际由 `S41dhcpcd`（dhcpcd 守护进程，走 DHCP）提供。`BR2_PACKAGE_NETWORK_MANAGER`/`CONNMAN` 均未开启，仅有 ifupdown + dhcpcd 两套机制。
- 目标：把静态 IP 直接“编译”进 rootfs 镜像。

### 2.2 做法（patch `0002`）

将静态配置放进 **`etc/network/interfaces.d/eth0`**（与在相似 rk3588 板上验证可用的方式一致；不要内联进主文件，避免任何文件位置/覆盖层面的歧义）：

```ini
# Static configuration for eth0 (ztl cm588) - baked via rk3588 fs-overlay
auto eth0
iface eth0 inet static
    address 192.168.80.202
    netmask 255.255.255.0
    gateway 192.168.80.254
```

主文件 `etc/network/interfaces` 保持 buildroot 自动生成的基线（`auto lo` + `source-directory /etc/network/interfaces.d`）。开机由 `S40network`（`/etc/init.d/S40network`，执行 `/sbin/ifup -a`）加载 `interfaces.d/` 下所有片段，把 `auto eth0` 拉起并配置地址与默认路由。

> ⚠️ **本 BSP 的 busybox 没有 ifupdown 小程序**：实测 `busybox-1.36.1` 的 `networking/Config.in` 中**不存在 `config IFUPDOWN` 符号**（仅有 `IFUPDOWN_IFSTATE_PATH`、`FEATURE_IFUPDOWN_*`、`IFUPDOWN_UDHCPC_CMD_OPTIONS` 等子项），`merge_config.sh` 会静默丢弃 `CONFIG_IFUPDOWN=y`。因此 `S40network` 执行 `/sbin/ifup -a` 时 busybox 报 “applet not found”，`interfaces.d/eth0` **永远不会被应用**——这也是早期“静态 IP 完全无效”的根因。
> 所以静态 IP **真正生效靠的是补丁新增的 `/etc/init.d/S45static-eth0`**：它在开机时直接用 busybox 的 `ip` 小程序（`CONFIG_IP=y`，本 SDK 已开启）配置 `192.168.80.202/24` 与默认路由。`interfaces.d/eth0` 仅作配置留存、不起实际作用；`dhcpcd.conf` 的 `denyinterfaces eth0` 仍须保留，避免 dhcpcd 抢管 eth0。

**`etc/dhcpcd.conf`**：在默认配置末尾加一行，避免 dhcpcd 与静态配置打架（WiFi 仍可走 DHCP）：

```ini
# --- ztl cm588: keep eth0 static, do not let dhcpcd manage it ---
denyinterfaces eth0
```

> 备选：若环境完全不需要 DHCP，也可直接禁用 `S41dhcpcd`；`denyinterfaces eth0` 更温和，保留 WiFi DHCP 能力。
> 若个别环境下 busybox `ifupdown` 的 `gateway` 没生效，可加兜底行：`up ip route add default via 192.168.80.254 dev eth0`。

### 2.3 重新生成 rootfs / update.img

```bash
cd /data/kkkqkx/rk3588bsp
./build.sh rootfs        # 重建 buildroot rootfs，应用 overlay → rootfs.ext2
./build.sh updateimg     # 把新 rootfs 打包进 update.img
```

- 仅改 rootfs，内核/uboot 不变；已成功烧过 6.1 的情况下，也可只刷 `rootfs` 分区。
- 产物路径：
  - rootfs 镜像：`buildroot/output/rockchip_rk3588/images/rootfs.ext2`
  - 打包软链：`output/firmware/rootfs.img` → 上述 `rootfs.ext2`
  - 最终升级包：`output/update/Image/update.img`（→ `output/firmware/update.img`）

> 镜像体积说明：本 `update.img` 约 1GB 属正常——其中 `rootfs.ext2` 占 ~894MB（完整 buildroot 根文件系统，已含本补丁的 `interfaces.d/eth0` 与 `dhcpcd.conf` 改动），`boot.img` 56MB（内核）、`uboot/recovery/oem/userdata/misc` 等合计 ~120MB。若拿它和带 Debian 的完整固件（数 GB）比会显得小，但分区镜像齐全、rootfs 完整，**产物无缺失**。实际使用的 `parameter.txt` 中 rootfs 为 `rootfs:grow`（占剩余全部空间），不存在被截断的可能。

### 2.4 验证

构建侧（无需上板）确认配置已进入镜像：

```bash
debugfs -R "cat /etc/network/interfaces.d/eth0" \
  buildroot/output/rockchip_rk3588/images/rootfs.ext2
# 应看到 eth0 静态段；主 interfaces 应为 lo + source-directory 基线
# dhcpcd.conf 末尾应有 denyinterfaces eth0
```

# 真正的生效脚本（busybox 无 ifupdown，静态 IP 靠它）
debugfs -R "cat /etc/init.d/S45static-eth0" \
  buildroot/output/rockchip_rk3588/images/rootfs.ext2
# 应看到：ip addr add 192.168.80.202/24 dev eth0 / ip route add default via 192.168.80.254

板端：

```bash
ip addr show eth0        # 192.168.80.202/24
ip route                 # default via 192.168.80.254 dev eth0
```

---

## 3. 产物 / 文件输出路径速查表

| 类别 | 路径 |
|---|---|
| 内核配置（使能补丁） | `kernel-6.1/arch/arm64/configs/ztl_defconfig`、`kernel-6.1/arch/arm64/configs/ztl_rk3588.config` |
| 内核配置接线（SDK） | `device/rockchip/common/configs/Config.in.kernel` |
| 设备树（板级） | `kernel-6.1/arch/arm64/boot/dts/rockchip/ztl-rk3588-cm.dts` |
| 内核/YT6801 驱动源码 | `kernel-6.1/drivers/net/ethernet/yt6801/`（`fuxi-gmac-*.c`） |
| AF_XDP / BPF 内核源码 | `kernel-6.1/net/xdp/`、`kernel-6.1/kernel/bpf/`、`kernel-6.1/arch/arm64/net/bpf_jit_comp.c` |
| rootfs 静态 IP overlay | `buildroot/board/rockchip/rk3588/fs-overlay/etc/network/interfaces.d/eth0`、`.../etc/network/interfaces`（基线）、`.../etc/dhcpcd.conf`、`.../etc/init.d/S45static-eth0`（**真正生效**：用 `ip` 小程序配静态地址，因本 BSP busybox 无 ifupdown） |
| boot 镜像 | `output/firmware/boot.img`（→ `output/update/Image/boot.img`） |
| buildroot rootfs 镜像 | `buildroot/output/rockchip_rk3588/images/rootfs.ext2` |
| rootfs 打包镜像 | `output/firmware/rootfs.img`（→ 上述 ext2） |
| 最终升级包 | `output/update/Image/update.img`（→ `output/firmware/update.img`） |
| 补丁 | `patches/rk3588/0001-kernel-6.1-enable-af_xdp-ebpf-for-mtl.patch`、`patches/rk3588/0002-rk3588-fs-overlay-eth0-static-ip.patch` |

---

## 4. 关联文档

- `RK3588_6.1_KERNEL_FULL_BUILD.md`：完整编译注意事项、执行命令、产物验证、烧录未生效排查。
- `RK3588_6.1_BOOT_IMG_ANALYSIS.md`：boot.img / 内核版本 / 配置取证分析。
