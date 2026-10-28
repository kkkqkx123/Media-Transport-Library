# RK3588 网络配置与双网口静态 IP 设置

> 文件名沿用历史的 `ETH0`，但内容已按实测更新为**双网口**方案：接口**不按名字绑定**，而是按驱动（`yt6801`）+ PCI BDF 识别，原因见 §2.2。

本笔记整理 RK3588（6.1 内核）上两类“网络相关”定制，供后续移植/排障参考：

1. **内核网络设置**：为 MTL `native_af_xdp` 后端开启的内核 AF_XDP / eBPF 配置项，以及它们在内核源码中的位置。
2. **双网口静态 IP**：因镜像无桌面端、内网无 DHCP，如何把两个 RJ45 的固定 IP 直接编译进 rootfs。

对应补丁都在仓库 `patches/rk3588/`：

| 补丁 | 内容 |
|---|---|
| `0001-kernel-6.1-enable-af_xdp-ebpf-for-mtl.patch` | 内核 AF_XDP/eBPF 使能（包含两个提交：defconfig 使能 + `STMMAC_UIO` 6.1 编译修复） |
| `0002-rk3588-fs-overlay-eth-static-ip.patch` | rootfs overlay：**双网口**静态 IP（主 `.202/24` + 默认路由 `.254`，备 `.201/24`），由 `S45static-net` 按 **驱动(yt6801)+PCI BDF** 绑定；`dhcpcd` 对 `eth0..eth3` 全部 deny |
| `0003-rk3588-ttyS3-second-console.patch` | DTS 双 console（`console=ttyS3,115200 console=ttyFIQ0`），串口排障用；**`&gmac1` 保持 disabled** |
| `0004-rk3588-fs-overlay-dual-serial-shell.patch` | fs-overlay `/etc/inittab`：`/dev/console`(ttyFIQ0) 与 `ttyS3` 各一个免登录 root shell |
| `0005-rk3588-debian-chroot-bash-fix.patch` | Debian 构建专用（chroot 显式 `/bin/bash`），buildroot 流程无关 |


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
ip -br link show                          # 记住哪个口是 yt6801（名字可能是 eth2/eth3，见 §2.2）
ls -l /sys/class/net/*/device/driver      # 确认驱动为 yt6801
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

## 2. 设置双网口静态 IP（rootfs overlay）

### 2.1 动机与实测机制

- 现象：镜像无桌面端，且部署内网无 DHCP 服务器 → 有线口拿不到地址。
- 实测（从构建产物确认，非猜测）：`BR2_INIT_BUSYBOX`（busybox init）；`/etc/network/interfaces` 默认只有 `lo` + `source-directory interfaces.d`，**无有线口静态配置**；地址实际由 `S41dhcpcd`（dhcpcd 守护进程，走 DHCP）提供。`BR2_PACKAGE_NETWORK_MANAGER`/`CONNMAN` 均未开启，仅有 ifupdown + dhcpcd 两套机制。
- 目标：把两个 RJ45 的静态 IP 直接“编译”进 rootfs 镜像。

### 2.2 做法（patch `0002`，2026-10-28 修订版）

真正生效的是 **`etc/init.d/S45static-net`**：它**不按名字**绑定，而是遍历 `/sys/bus/pci/devices/*/net/*`，只保留驱动为 `yt6801` 的网口，按 PCI BDF 从小到大：

| 顺序 | 地址 | 说明 |
|---|---|---|
| 第一个（BDF 最小） | `192.168.80.202/24` + 默认路由 `192.168.80.254` | 主口 |
| 第二个 | `192.168.80.201/24`（不设默认路由） | 备口，避免抢网关 |

找不到 `yt6801` 网口时回退到 `eth0`/`eth1`。每次赋值都会往 `/dev/kmsg` 打一行 `S45static-net: <if> -> <addr>`，串口日志里可直接看到它到底配给了谁。

> **为什么不能写死 `eth0/eth1`（2026-10-28 实测踩坑）**：SoC dtsi 里有 `aliases { ethernet0 = &gmac0; ethernet1 = &gmac1; }`，Linux 按这个别名给 GMAC 网口定名。一旦某个 GMAC 能 probe，它就会占掉 `eth0/eth1`，把真正的两个 RJ45（PCIe yt6801）挤到 `eth2/eth3`——于是"给 eth0 配了 .202"实际配到了没插网线的口上，表现为 **IP 有、ping 不通**。同一轮里打开 `&gmac1` 还把 uart3 的引脚抢走（见 `docs/dev/RK3588_TTYS3_CONSOLE_ENABLE.md` §10.1），所以本板**必须保持 `&gmac1` disabled**。

`etc/network/interfaces` 与 `interfaces.d/eth0`、`interfaces.d/eth1` 仍随补丁提供，但**仅作配置留存**：本 BSP 的 busybox 没有 ifupdown 小程序，`S40network` 的 `/sbin/ifup -a` 无法应用它们（见下方警告）。

> ⚠️ **本 BSP 的 busybox 没有 ifupdown 小程序**：实测 `busybox-1.36.1` 的 `networking/Config.in` 中**不存在 `config IFUPDOWN` 符号**（仅有 `IFUPDOWN_IFSTATE_PATH`、`FEATURE_IFUPDOWN_*`、`IFUPDOWN_UDHCPC_CMD_OPTIONS` 等子项），`merge_config.sh` 会静默丢弃 `CONFIG_IFUPDOWN=y`。因此 `S40network` 执行 `/sbin/ifup -a` 时 busybox 报 “applet not found”，`interfaces.d/eth0` **永远不会被应用**——这也是早期“静态 IP 完全无效”的根因。
> 所以静态 IP **真正生效靠的是补丁新增的 `/etc/init.d/S45static-net`**：它在开机时直接用 busybox 的 `ip` 小程序（`CONFIG_IP=y`，本 SDK 已开启）按驱动 + PCI BDF 识别网口并配置地址。`interfaces.d/*` 仅作配置留存、不起实际作用；`dhcpcd.conf` 的 `denyinterfaces eth0 eth1 eth2 eth3` 仍须保留，避免 dhcpcd 抢管这些口。

**`etc/dhcpcd.conf`**：在默认配置末尾加一行，避免 dhcpcd 与静态配置打架（WiFi 仍可走 DHCP）：

```ini
# --- ztl cm588: all wired ports get static addresses from S45static-net ---
# eth 编号取决于是否有 GMAC 抢到 ethernetN 别名，所以整段 deny 而不是只 deny 两个名字
denyinterfaces eth0 eth1 eth2 eth3
```

> 备选：若环境完全不需要 DHCP，也可直接禁用 `S41dhcpcd`；`denyinterfaces` 更温和，保留 WiFi DHCP 能力。
> 默认路由已由 `S45static-net` 里的 `ip route add default via 192.168.80.254 dev <主口>` 设置，不需要在 `interfaces.d/*` 里再兜底（那份文件本就不会被执行）。

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

> 镜像体积说明：本 `update.img` 约 1GB 属正常——其中 `rootfs.ext2` 占 ~894MB（完整 buildroot 根文件系统，已含本补丁的 `S45static-net` 与 `dhcpcd.conf` 改动），`boot.img` 56MB（内核）、`uboot/recovery/oem/userdata/misc` 等合计 ~120MB。若拿它和带 Debian 的完整固件（数 GB）比会显得小，但分区镜像齐全、rootfs 完整，**产物无缺失**。实际使用的 `parameter.txt` 中 rootfs 为 `rootfs:grow`（占剩余全部空间），不存在被截断的可能。

### 2.4 验证

构建侧（无需上板）确认配置已进入镜像：

```bash
IMG=buildroot/output/rockchip_rk3588/images/rootfs.ext2

# 真正生效的脚本（busybox 无 ifupdown，静态 IP 靠它）
debugfs -R "cat /etc/init.d/S45static-net" $IMG
# 应看到 addr_primary=192.168.80.202 / addr_secondary=192.168.80.201 / gw=192.168.80.254
# 以及 /sys/bus/pci/devices/*/net/* + drv = yt6801 的识别逻辑

# 仅作留存的 ifupdown 配置（busybox 无 ifupdown，不会被应用）
debugfs -R "cat /etc/network/interfaces.d/eth0" $IMG
debugfs -R "cat /etc/network/interfaces.d/eth1" $IMG
# 主 interfaces 应为 lo + source-directory 基线

# dhcpcd 不得抢占任何有线口
debugfs -R "cat /etc/dhcpcd.conf" $IMG | tail -1   # denyinterfaces eth0 eth1 eth2 eth3
```

板端（`<if>` 以 `ip -o link` 里驱动为 `yt6801` 的那个为准，BDF 最小的拿到 `.202`）：

```bash
ip -o link                                        # 找到两个 yt6801 口
cat /dev/kmsg | grep S45static-net                # 应有两行 S45static-net: <if> -> 192.168.80.20x
ip -br addr show                                  # 一个 .202/24、一个 .201/24
ip route                                          # default via 192.168.80.254 dev <主口>
ping -c3 192.168.80.254                           # 插线的那口应通
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
| rootfs 静态 IP overlay | `buildroot/board/rockchip/rk3588/fs-overlay/etc/init.d/S45static-net`（**真正生效**：按驱动 `yt6801` + PCI BDF 配双口地址，busybox `ip`）、`.../etc/dhcpcd.conf`（`denyinterfaces eth0 eth1 eth2 eth3`）、`.../etc/network/interfaces.d/eth0`、`.../etc/network/interfaces.d/eth1`、`.../etc/network/interfaces`（后三者**仅留存**，本 BSP busybox 无 ifupdown，`S40network` 无法应用） |
| boot 镜像 | `output/firmware/boot.img`（→ `output/update/Image/boot.img`） |
| buildroot rootfs 镜像 | `buildroot/output/rockchip_rk3588/images/rootfs.ext2` |
| rootfs 打包镜像 | `output/firmware/rootfs.img`（→ 上述 ext2） |
| 最终升级包 | `output/update/Image/update.img`（→ `output/firmware/update.img`） |
| 补丁 | `patches/rk3588/0001-kernel-6.1-enable-af_xdp-ebpf-for-mtl.patch`、`patches/rk3588/0002-rk3588-fs-overlay-eth-static-ip.patch`（双网口）、`patches/rk3588/0003-rk3588-ttyS3-second-console.patch`、`patches/rk3588/0004-rk3588-fs-overlay-dual-serial-shell.patch` |

---

## 4. 关联文档

- `RK3588_6.1_KERNEL_FULL_BUILD.md`：完整编译注意事项、执行命令、产物验证、烧录未生效排查。
- `RK3588_6.1_BOOT_IMG_ANALYSIS.md`：boot.img / 内核版本 / 配置取证分析。
