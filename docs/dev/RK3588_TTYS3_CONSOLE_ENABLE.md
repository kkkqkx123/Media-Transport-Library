# 让 `/dev/ttyS3` 具备 `/dev/ttyFIQ0` 同等能力（buildroot，kernel-6.1）

> 前置阅读：`docs/hw/RK3588_UART_CONSOLE_AND_SERIAL_ACCESS.md`（本板串口/控制台现状）
> 采集对象：`devs:/data/kkkqkx/rk3588bsp`，板级 DTS `ztl-rk3588-cm`，内核 `kernel-6.1`
> 文中路径除特别说明外均相对 BSP 仓库根目录。

---

## 1. 结论摘要

`ttyS3` 与 `ttyFIQ0` 的差距**不在驱动、不在引脚、不在内核 config**，只差两处绑定：

| 差距 | 现状 | 需要的改动 |
|---|---|---|
| 内核 console 没绑到 ttyS3 | cmdline 只有 `console=ttyFIQ0` | DTS `chosen/bootargs` 追加 `console=ttyS3,115200`（**内核改动仅此一处**） |
| rootfs 没在 ttyS3 起 getty/shell | inittab 只在 `/dev/console` 上起 `-/bin/sh` | 追加一行 getty（或让 console 落到 ttyS3 后沿用现成的 shell） |

即：**内核侧只需改 1 行 cmdline，驱动/引脚/配置零改动**；其余工作都在 rootfs。

> ⚠️ **2026-10-29 修正**：上面这条只在 bootloader 不覆盖 DTB 时成立。本 SDK 的 u-boot 会用
> 自己的 env `bootargs` **整体替换** `chosen/bootargs`，`console=` 全丢 → 内核无 console、
> 串口完全静默。**必须同时应用 `0006-rk3588-uboot-keep-ttyS3-console.patch`**，详见 §10.4。

前提事实（均已核对，见 §3）：`uart3` 在板级 DTS 里已 `status = "okay"`，8250 驱动已注册 `/dev/ttyS3`，`CONFIG_SERIAL_8250_CONSOLE=y` 已开。

---

## 2. 先把 `ttyFIQ0` 的"能力"拆开

"和 ttyFIQ0 一样"其实是 6 件事的合集：

| # | 能力 | 由什么提供 | ttyS3 现状 |
|---|---|---|---|
| 1 | 硬件串口 + pinmux | DTS `&uart3` + `uart3m1_xfer` | ✅ 已具备 |
| 2 | tty 设备节点 | 8250 / `8250_dw` 驱动 | ✅ `/dev/ttyS3` 开机即存在 |
| 3 | 内核日志输出 | cmdline `console=` | ❌ 只有 `console=ttyFIQ0` |
| 4 | `/dev/console`（shell 落点） | cmdline **最后一个** `console=` | ❌ 现在是 ttyFIQ0 |
| 5 | 交互 shell / getty | rootfs `inittab`（buildroot） | ❌ 只挂在 `/dev/console` |
| 6 | 早期启动日志 | cmdline `earlycon=` | ❌ 指向 UART2（`0xfeb50000`） |
| 7 | FIQ 级调试（中断上下文调试壳、NO_SLEEP） | `fiq-debugger` 专有 | ❌ 无法等价，见 §8 |

---

## 3. 现状核对（证据）

| 事实 | 证据 |
|---|---|
| uart3 已启用 | `kernel-6.1/arch/arm64/boot/dts/rockchip/ztl-rk3588-cm.dts:538-540`：`&uart3 { status = "okay"; };` |
| uart3 用 m1 复用（未在板级覆盖，继承 SoC 默认） | `rk3588s.dtsi:7117-7128`：`pinctrl-0 = <&uart3m1_xfer>;` |
| 引脚 = GPIO3_B5(TX) / GPIO3_B6(RX) | `rk3588s-pinctrl.dtsi:3132-3138` |
| 编号就是 ttyS3 | `rk3588s.dtsi:65-74`：`serial3 = &uart3;`（8250 按 alias 定编号） |
| 8250 控制台能力已开 | `kernel-6.1/.config:2781-2793`：`SERIAL_8250=y`、`SERIAL_8250_DW=y`、`SERIAL_8250_CONSOLE=y`、`NR_UARTS=32`、`RUNTIME_UARTS=15` |
| 当前 console 只有 ttyFIQ0 | `rk3588-linux.dtsi:15`：`earlycon=uart8250,mmio32,0xfeb50000 console=ttyFIQ0 ...` |
| rootfs 只有 console 上的 shell | `buildroot/output/rockchip_rk3588/target/etc/inittab:31`：`::respawn:-/bin/sh # ttyS0::respawn:/sbin/getty ... # GENERIC_SERIAL` |
| 该 shell 的来源开关 | `buildroot/output/rockchip_rk3588/.config:516`：`BR2_TARGET_SERIAL_SHELL_SH=y`（`busybox.mk:252-267` 据此生成上面那行） |

---

## 4. 方案 A（推荐）：内核侧只改 cmdline

### 4.1 改哪里

`chosen/bootargs` 定义在 `rk3588-linux.dtsi:14-16`，该文件被所有板共用，**不建议直接改**。改为在板级 DTS 覆盖 `/chosen`（BSP 内已有同类先例，如 `rk3588-nvr-demo-v10-spi-nand.dts:16-18`）：

```dts
/ {
	/* ttyS3 = 第二控制台（日志）；ttyFIQ0 放最后，保住"必定存在"的 root shell */
	chosen {
		bootargs = "earlycon=uart8250,mmio32,0xfeb50000 console=ttyS3,115200 console=ttyFIQ0 \
			    irqchip.gicv3_pseudo_nmi=0 root=PARTUUID=614e0000-0000 rw rootwait \
			    rcupdate.rcu_expedited=1 rcu_nocbs=all";
	};
};
```

位置：`kernel-6.1/arch/arm64/boot/dts/rockchip/ztl-rk3588-cm.dts` 的顶层 `/` 节点内（文件里已有 `model`/`compatible` 的 `/` 节点，追加一个 `chosen { ... }` 即可）。

可行性已实测验证：

- dtc 对同一节点的同名属性**取最后一次定义**（`printf '/dts-v1/;\n/ { chosen { bootargs = "first"; }; };\n/ { chosen { bootargs = "second"; }; };\n' | dtc -I dts -O dts` → 输出 `bootargs = "second"`），所以后 include 的板级 DTS 能覆盖 dtsi 的 `chosen`。
- BSP 内已有同款先例：`rk3588-nvr-demo-v10-spi-nand.dts` 第 10 行 `#include "rk3588-linux.dtsi"`，第 16-17 行又重定义了 `chosen/bootargs`。

### 4.2 这样做的效果

- **日志**：两个 console 都收到 printk（`/proc/consoles` 会列出两个）。
- **`/dev/console`**：指向**最后一个** `console=`，即 `ttyFIQ0`（UART2 调试口）。
- **shell**：buildroot 的 `::respawn:-/bin/sh` 打开 `/dev/console` → shell 落在 **ttyFIQ0**，这是**永远存在的兜底口**（UART2 一定接了调试线、一定能出日志）。
- ttyS3 的 shell 由 rootfs 侧显式给出（§5，已实施为 `0004` 补丁），因此两个口最终都能进命令行。

> 关键权衡：如果把 `console=ttyFIQ0` 写在最后一位之外（即 ttyS3 是最后一个），shell 会跟着落到 ttyS3——一旦 uart3 的 pinmux 或接线有问题（2026-10-28 就踩到了，见 §10.1），板子将**一个可用控制台都没有**。所以本方案把 ttyFIQ0 放最后，ttyS3 的 shell 用 inittab 显式补。

### 4.3 注意事项

| 项 | 说明 |
|---|---|
| 波特率必须显式写 | `console=ttyS3` 不写波特率 → 沿用 tty 默认 **9600**；写成 `console=ttyS3,115200` 才是 115200，且要与 USB-TTL 端一致 |
| 不要只写 `console=ttyS3` 而删掉 `console=ttyFIQ0` | 会丢掉调试口日志；除非按 §6 明确要"替代" |
| `earlycon` 仍是 UART2 | 想在最早期就看 ttyS3，改成 `earlycon=uart8250,mmio32,0xfeb60000`（uart3 基址 `0xfeb60000`） |
| 编译 | 只改 DTS，`./build.sh kernel` 即可产出新 `boot.img`（FIT 内嵌 dtb），无需动内核 config |

---

## 5. rootfs 侧：给 ttyS3 也起一个免登录 shell（已实施为补丁 `0004`）

内核 console 由 §4 处理；ttyS3 的交互 shell 直接在 fs-overlay 的 `/etc/inittab` 里补，**不需要 getty/login**：

```
::respawn:-/bin/sh
ttyS3::respawn:-/bin/sh
```

busybox init 的 respawn 行会按 id 字段打开 `/dev/ttyS3` 并在其上跑 shell（`-` 前缀 = login shell），效果与 ttyFIQ0 上 buildroot 默认的 `::respawn:-/bin/sh` 完全一致：**免登录 root shell**。ttyS3 的波特率由 `console=ttyS3,115200` 在启动时设定，无需再 `stty`。

要点：

- 必须提供**整份** inittab（busybox init 不做片段合并），模板在 `buildroot/package/busybox/inittab`，overlay 路径 `buildroot/board/rockchip/rk3588/fs-overlay/etc/inittab`（`BR2_ROOTFS_OVERLAY` 已生效，见 `buildroot/output/rockchip_rk3588/.config:530`）。
- **故意不写 `# GENERIC_SERIAL` 标记行**：buildroot 的 `BUSYBOX_SET_GETTY` 钩子（`busybox.mk:265-269`）会把该行替换成 `::respawn:-/bin/sh` 或 getty 行；若保留标记行，会多出一个 shell 跟 `/dev/console` 抢 UART2。（这与"想用 BR2_TARGET_GENERIC_GETTY 时必须保留标记行"正好相反，二者择一。）
- 用 `ttyS3::respawn:-/bin/sh` 而不是 `getty`：绕过 login/口令/termios 差异，行为与 ttyFIQ0 一致。

### 5.2 用 buildroot 配置项（更"正统"）

等价做法是改 buildroot 配置（`System configuration`）：

```
BR2_TARGET_GENERIC_GETTY=y
BR2_TARGET_GENERIC_GETTY_PORT="ttyS3"
BR2_TARGET_GENERIC_GETTY_BAUDRATE=115200
BR2_TARGET_GENERIC_GETTY_TERM="vt100"
```

`busybox.mk:252` 会据此把 inittab 里 `# GENERIC_SERIAL` 那行替换成 `ttyS3::respawn:/sbin/getty -L ttyS3 115200 vt100`（同时 `::respawn:-/bin/sh` 不再生成）。缺点是一次只能配一个口。

---

## 6. 方案 C：让 ttyS3 完全取代 ttyFIQ0

如果目标是把 UART2 解放出来（或彻底不用 fiq-debugger）：

1. `rk3588-linux.dtsi` / 板级 DTS 里把 `fiq-debugger` 置 `status = "disabled"`（或把 `rockchip,serial-id` 改成 `<-1>`，驱动会走 `rk_serial_debug_init_dummy()`，`rk_fiq_debugger.c:1060`）。
2. cmdline 去掉 `console=ttyFIQ0`，只留 `console=ttyS3,115200`；`earlycon` 改 `0xfeb60000`。
3. 这时 uart2 不再被占用，若还需要这路串口，可给 `&uart2 { status = "okay"; }`（它就会以 `/dev/ttyS2` 出现）。

代价：失去 fiq-debugger 的 FIQ 级调试能力（§8），且 `ttyFIQ0` 设备节点消失。

> 注意顺序：**必须先禁 fiq-debugger，再启用 uart2**，否则 `rk_fiq_debugger.c:1095` 会打印 `uart2 is enabled, please disable it` 并放弃接管。

---

## 7. 引脚可用性核查：GPIO3_B6 是否被 Type-C 抢占

uart3 m1 的 RX 是 **GPIO3_B6**，而 evb7 参考设计的 Type-C 中断脚**原本也在 GPIO3_B6**：

- `rk3588-evb7-v11.dtsi:583-594`：`husb311@4e`（i2c6）用 `interrupt-parent = <&gpio3>; interrupts = <RK_PB6 ...>; pinctrl-0 = <&usbc0_int>;`
- `rk3588-evb7-v11.dtsi:819-822`：`usbc0_int = <3 RK_PB6 RK_FUNC_GPIO ...>` ← 与 uart3_rx 同脚

**好消息：CM588 板级已经把这两处都改掉了**：

- `ztl-rk3588-cm.dts:157-166`：把 Type-C 控制器换成 `fusb302@22`，中断改到 `interrupt-parent = <&gpio0>; interrupts = <RK_PD3 ...>`；
- `ztl-rk3588-cm.dts:290-292`：直接把 `usbc0_int` 重定义为 `<0 RK_PD3 RK_FUNC_GPIO &pcfg_pull_up>`；
- `ztl-rk3588-cm.dts:225`：`/delete-node/ husb311@4e;` 移除原节点。

反编译实际 dtb 交叉验证（`dtc -I dtb -O dts .../ztl-rk3588-cm.dtb`）：

- `usbc0-int` 的 pins = `<0x0 0x1b 0x0 ...>` → bank0 pin 27 = **GPIO0_D3**；
- `uart3m1-xfer` 的 pins = `<0x3 0xe 0xa ... 0x3 0xd 0xa ...>` → bank3 pin 14/13 = **GPIO3_B6 / GPIO3_B5**；
- `serial@feb60000` 的 `status = "okay"`。

结论：**GPIO3_B6 在本板上没有第二方占用，uart3 可以正常收发**（若沿用未改动的 evb7 参考设计则必然冲突）。

---

## 8. 和 ttyFIQ0 仍存在的差异

| 差异 | 说明 |
|---|---|
| FIQ/中断上下文调试壳 | fiq-debugger 提供 fiq 级别的调试入口（可在中断上下文、甚至系统半死时使用），普通 8250 console 做不到 |
| NO_SLEEP 轮询输出 | `CONFIG_FIQ_DEBUGGER_NO_SLEEP=y`（`kernel-6.1/.config:6379`）让它在原子上下文不睡眠；8250 console 走常规路径 |
| SysRq | `CONFIG_MAGIC_SYSRQ=y` 但 `MAGIC_SYSRQ_DEFAULT_ENABLE=0`（`ztl_defconfig:695-696`）。要在 ttyS3 上用 break + SysRq，需 cmdline 加 `sysrq_always_enabled=1`，或运行期 `echo 1 > /proc/sys/kernel/sysrq` |
| 波特率/流控 | ttyS3 由 termios 决定（`stty`/getty 参数），fiq-debugger 固定 1500000 且不支持流控 |
| 设备名稳定性 | `ttyFIQ0` 与硬件号无关、永远存在；`ttyS3` 依赖 `serial3` alias，改 DTS 别名会变号 |

---

## 9. 验证步骤

板端（接在 UART3 的 USB-TTL 上）：

```sh
cat /proc/cmdline                  # 期望出现 console=ttyS3,115200
cat /proc/consoles                 # 期望 ttyFIQ0 与 ttyS3 两条
cat /sys/class/tty/console/active  # 期望 ttyFIQ0（最后一个 console=）
ls -l /dev/ttyS3                   # 设备节点存在
cat /proc/1/cmdline                # 交互 shell 存在
# 纯数据回环（不需要 shell）：
stty -F /dev/ttyS3 115200 raw -echo && echo hello > /dev/ttyS3
```

内核日志侧期望：早期 `printk` 同时出现在两路；`dmesg | grep -i "console"` 可见两个 console 注册。

---

## 10. 落地记录与首次上板失败复盘

### 10.1 2026-10-28 首次上板：ttyS3 完全无输出，根因是 gmac1 抢了 uart3 的引脚

第一版只做了"cmdline 追加 `console=ttyS3,115200` 并把 shell 放过去"，同时网口补丁里把 `&gmac1` 打开了。结果：**ttyS3 一个字符都没有**，`eth1` 起不来、`eth0` 有地址却不可达。

根因（同一处引脚冲突，两个症状）：

| 现象 | 机制 |
|---|---|
| ttyS3 静默 | `gmac1_tx_bus2` 把 **GPIO3_B5**（`gmac1_txen`）抢走，而 GPIO3_B5 正是 `uart3m1` 的 **TX**（`rk3588s-pinctrl.dtsi:464-471` vs `:3132-3138`）。pad 被 mux 成 GMAC 功能后，8250 即使注册了 console 也发不出任何字符。即使 gmac1 probe 失败，Rockchip pinctrl 释放时也不回滚 mux 寄存器，所以"没输出"而不是"乱码" |
| eth1 起不来 | DT 别名 `ethernet1 = &gmac1`，gmac1 被点名占用 eth1，而板上没有 mdio1 上的第二颗 PHY（`rk3588-evb7-v11.dtsi` 的 `rgmii_phy`） |
| eth0 有 IP 不通 | `ethernet0 = &gmac0`，gmac0 拿到 eth0 但网线插在 PCIe 的 yt6801 口上（真 RJ45 被挤到 `eth2/eth3`） |

附带损失：`gmac1` 的 RGMII 组还复用了 GPIO3_PA0/PA1/PB3/PB4，与板级 `open-gpio-pins`（`ztl-rk3588-cm.dts` 自定义 pinctrl）重叠。

**结论：本板 `gmac1` 与 `uart3` 是硬互斥，且 CM588 的两个 RJ45 是 PCIe yt6801，gmac1 无用 → `&gmac1` 必须保持 `disabled`。**

### 10.2 最终落地（补丁 + 构建）

| 补丁 | 内容 |
|---|---|
| `0001-kernel-6.1-enable-af_xdp-ebpf-for-mtl.patch` | 内核 AF_XDP/eBPF + `ztl_defconfig`（含 `STMMAC_UIO` 修复） |
| `0002-rk3588-fs-overlay-eth-static-ip.patch` | fs-overlay：`S45static-net` 按 **驱动(yt6801)+PCI BDF** 绑定双网口（`.202` 主 + `.201` 备），`dhcpcd deny eth0..eth3`；**不再动 gmac1** |
| `0003-rk3588-ttyS3-second-console.patch` | DTS `chosen/bootargs`：`console=ttyS3,115200 console=ttyFIQ0`（ttyFIQ0 最后 = 兜底 shell），`&gmac1` 保持 `disabled` |
| `0004-rk3588-fs-overlay-dual-serial-shell.patch` | fs-overlay `/etc/inittab`：`::respawn:-/bin/sh` + `ttyS3::respawn:-/bin/sh` |
| `0005-rk3588-debian-chroot-bash-fix.patch` | Debian 构建专用（chroot 显式 `/bin/bash`），buildroot 流程不需要 |
| `0006-rk3588-uboot-keep-ttyS3-console.patch` | **必需（2026-10-29 追加）**：u-boot `board_fdt_chosen_bootargs()` 把 `console=ttyFIQ0 console=ttyS3,115200 earlycon=…0xfeb60000` 追加进 env `bootargs`，避免 DTB 的 `console=` 被整体覆盖；无此补丁串口必定静默 |

BSP 状态：HEAD = `ab0040897`，工作区 = 基线 + `0001`~`0004`（未提交）；四个补丁依次 `git apply --cached` 后与工作区 `git diff` 为空。

构建产物（`devs:/data/kkkqkx/rk3588bsp`）：

| 产物 | 说明 |
|---|---|
| `kernel-6.1/boot.img` | FIT，dtb 已含双 console，`&gmac1` = disabled |
| `buildroot/output/rockchip_rk3588/images/rootfs.ext2` | 含新 `S45static-net` 与双 shell `inittab` |
| `output/update/Image/update.img` | RKFW，约 1.09 GB，可直接烧录 |

产物校验（`debugfs` / `dtc` 实测）：

- dtb：`bootargs` 含 `console=ttyS3,115200 console=ttyFIQ0`；`ethernet@fe1c0000`（gmac1）`status = "disabled"`；`serial@feb60000`（uart3）`status = "okay"`；
- rootfs：`/etc/inittab` 两行 respawn（console + ttyS3）；`S45static-net` 为驱动/BDF 版本。

### 10.3 上板验证（表格已被 §10.4 推翻，见该节）

| 通道 | 参数 | 期望 |
|---|---|---|
| UART2 调试口（GPIO0_B5/B6） | 1500000 8N1 | 内核日志 + **root shell**（兜底，必定存在） |
| UART3（GPIO3_B5/B6，TTL 抽头） | 115200 8N1 | 内核日志 + **root shell**（来自 inittab） |

网络侧串口里可自查：`ip -o link`、`dmesg | grep -i "yt6801\|eth"`、看 `S45static-net` 打在 `/dev/kmsg` 的 `S45static-net: <if> -> <addr>` 行。

### 10.4 2026-10-29：仍然完全静默，真正根因是 **u-boot 覆盖了 DTB 的 bootargs**

10.1 修掉 `gmac1` 之后 ttyS3 依旧没有任何输出；同一板子在 5.10 树上（同一份 u-boot、同样只改 DTS）
也一模一样。逐层核对后确认：**DTS 的 `chosen/bootargs` 在内核启动前就被 u-boot 整体换掉了**。

链路（`u-boot/arch/arm/mach-rockchip/board.c`）：

1. `board_fdt_chosen_bootargs()` 是**强符号**（不是 `common/fdt_support.c` 里那个 `__weak` 版本），
   依次调用 `bootargs_add_dtb_dtbo/partition/fwver/android()`；
2. `CONFIG_ROCKCHIP_PRELOADER_ATAGS=y`，而 `bootargs_add_fwver()` 里 `list2`（`uboot-<版本>-<日期>`）
   恒非空 → **无条件** `env_update("bootargs", "androidboot.fwver=...")`；
3. `env_update()`（`cmd/nvedit.c`，注释 "If varname doesn't exist, create it and set varvalue"）
   在变量不存在时**创建**它 → env `bootargs` 只剩 `androidboot.*`，**一个 `console=` 都没有**；
4. `fdt_chosen()`（`common/fdt_support.c:337`）拿到 `env_get("bootargs")` 后
   `fdt_setprop(chosen, "bootargs", str, strlen(str)+1)` —— **整体替换**，DTS 里写的
   `console=ttyS3,115200 console=ttyFIQ0` 全部丢失；
5. 内核命令行里没有任何 `console=` → **不注册任何 console** → 串口完全静默，也没有 shell。

> 因此 §1 里"内核侧只需改 1 行 cmdline"只在 bootloader 不覆盖 DTB 时成立；本 SDK 不成立。

修复：`0006-rk3588-uboot-keep-ttyS3-console.patch`，在 `board_fdt_chosen_bootargs()` 的 `return` 前
往**同一个** env 变量追加 console：

```c
	env_update("bootargs",
		   "console=ttyFIQ0 console=ttyS3,115200 earlycon=uart8250,mmio32,0xfeb60000");
	/* env_update() 可能重分配，必须重新取指针，否则 bootargs 悬空 */
	bootargs = env_get("bootargs");
```

配套调整（`0003` 已同步修改）：

- `console=` 顺序**反转**为 `console=ttyFIQ0 console=ttyS3,115200`：本板 UART2 FIQ **未引出**，
  必须让 ttyS3 成为最后一个 console（`/dev/console` 落到 UART3），否则 shell 起在看不见的口上；
- `earlycon` 由 `0xfeb50000`（uart2）改为 `0xfeb60000`（uart3），早期日志才落在能看到的端口。

上板预期（取代 §10.3 表格）：

| 通道 | 参数 | 期望 |
|---|---|---|
| UART2 调试口 | 1500000 8N1 | **无输出**——本板未引出，u-boot 自身日志（`CONFIG_BAUDRATE=1500000`）也走这里 |
| UART3（GPIO3_B5/B6，TTL） | 115200 8N1 | **从内核 earlycon 行开始**有日志；随后 systemd 在 ttyS3 上起 getty |

若按 §10.4 修完 UART3 仍无输出，才轮到物理层：TTL 侧 TX/RX 需交叉、载板 ttl/232/485 跳线是否在 TTL、
波特率是否 115200 8N1。

---

## 11. 仍未覆盖

| 项 | 说明 |
|---|---|
| UART3 物理引出 | 若撤掉 gmac1 后 ttyS3 仍静默，则问题不在 pinmux，而在载板接线/抽头（改试 uart0 `ttyS0` 或 uart7 `ttyS7`，或换 `uart3m2` = GPIO4_A5/A6） |
| gmac 是否真的没有 PHY | 本结论基于"两个 RJ45 是 yt6801"的实测；若硬件后续改了网口方案，需要重新评估 `ethernetN` 别名与 `S45static-net` 的绑定策略 |
