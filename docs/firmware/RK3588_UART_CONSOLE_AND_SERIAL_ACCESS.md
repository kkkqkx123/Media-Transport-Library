# RK3588（ztl CM588）调试串口与命令行访问通道

> 采集对象：`devs:/data/kkkqkx/rk3588bsp`（Rockchip RK3588 6.1 SDK，板级 DTS `ztl-rk3588-cm`）
> 采集时间：2026-10-28
> 关联文档：`docs/dev/RK3588_TTYS3_CONSOLE_ENABLE.md`（如何让 `ttyS3` 具备同等能力）、`docs/env/RK3588_BOARD_ENV.md`（板上环境实测）
> 文中路径除特别说明外，均相对 BSP 仓库根目录。

---

## 1. 一句话结论

调试控制台是 **UART2 → `/dev/ttyFIQ0`（1500000 8N1，TX=GPIO0_B5 / RX=GPIO0_B6）**，**不是 `/dev/ttyS2`**。
buildroot 镜像在这一路上电即给 **root shell**（无登录、无桌面依赖）；普通串口 `/dev/ttyS0`、`ttyS3`、`ttyS7` 虽然存在，但默认只有数据通道，**没有 shell**。

"能否拿到命令行"取决于两件独立的事：**内核 console 绑在哪个 tty** + **rootfs 在哪个 tty 起了 getty/shell**。二者默认都指向 `ttyFIQ0`。

---

## 2. 四个层次要分开看

| 层 | 决定者 | 本板取值 | 证据 |
|---|---|---|---|
| Bootloader 控制台 | U-Boot DT + 配置 | UART2，1500000 | `u-boot/arch/arm/dts/rk3588-u-boot.dtsi:16`（`stdout-path = &uart2`）、`u-boot/configs/rk3588_defconfig:185`（`CONFIG_BAUDRATE=1500000`） |
| 内核 earlycon（启动最早期日志） | cmdline | UART2（`0xfeb50000`） | `kernel-6.1/arch/arm64/boot/dts/rockchip/rk3588-linux.dtsi:15` |
| 内核 console（printk 输出） | cmdline | `ttyFIQ0` | 同上 |
| 用户态命令行（getty / shell） | rootfs | buildroot：`/dev/console` 上直接 `/bin/sh` | `buildroot/output/rockchip_rk3588/target/etc/inittab:31` |

关键机制：**`/dev/console` 指向 cmdline 里最后一个 `console=` 设备**；buildroot 的 shell 挂在 `/dev/console` 上。所以"日志打到哪里"和"命令行在哪个口"由同一组参数决定——用多个 `console=` 可以让日志多路输出，而交互终端只跟随最后一个。

---

## 3. 为什么是 `ttyFIQ0` 而不是 `ttyS2`

1. `rk3588-linux.dtsi:42-53` 定义 `fiq-debugger`：`rockchip,serial-id = <2>`（占 UART2）、`rockchip,baudrate = <1500000>`、`status = "okay"`。
2. 板级 DTS 只为 `&uart2` 配了 pinctrl（`ztl-rk3588-cm.dts:551-555`，注释 `//debug uart`），**没有** `status = "okay"`；SoC dtsi 里 uart2 默认 `disabled`。因此 8250 驱动不注册 `ttyS2`。
3. 若反过来把 uart2 打开，fiq-debugger 会直接拒载：`kernel-6.1/drivers/soc/rockchip/fiq_debugger/rk_fiq_debugger.c:1095` 打印 `uart%d is enabled, please disable it` 并返回 `-EINVAL`。
4. fiq-debugger 注册的 tty 驱动名固定为 `ttyFIQ`（`fiq_debugger.c:1355`），实例号取 `pdev->id = rk_fiq_debugger_id++`，从 0 开始（`rk_fiq_debugger.c:977`、`:1016`）→ **设备名与硬件 UART 号无关**，永远是 `ttyFIQ0`（只有一个实例）。
5. 内核配置保证它接管 console：`kernel-6.1/.config:6378-6386`（`FIQ_DEBUGGER`、`FIQ_DEBUGGER_CONSOLE`、`FIQ_DEBUGGER_CONSOLE_DEFAULT_ENABLE`、`ROCKCHIP_FIQ_DEBUGGER`）。

---

## 4. 引脚与串口参数

| 串口 | mux 组 | TX | RX | 备注 |
|---|---|---|---|---|
| **UART2（调试口）** | `uart2m0` | **GPIO0_B5** | **GPIO0_B6** | `rk3588s-pinctrl.dtsi:3080-3086`；参数 **1500000 8N1** |
| UART3 | `uart3m1` | GPIO3_B5 | GPIO3_B6 | `rk3588s-pinctrl.dtsi:3132-3138`；DTS 注释 `/* cm588 ttl/232/485 part */` |
| UART0 | `uart0m2` | GPIO4_A3 | GPIO4_A4 | `rk3588s-pinctrl.dtsi:3007-3013` |
| UART7 | `uart7m1` | GPIO3_C0 | GPIO3_C1 | `rk3588s-pinctrl.dtsi:3300-3306` |
| UART8 | `uart8m1`（含 ctsn） | GPIO3_A2 | GPIO3_A3 | `rk3588s-pinctrl.dtsi:3357-3370`；蓝牙占用 |

> UART3 在板上是 TTL/232/485 复合口，接 USB-TTL 前务必按原理图确认取的是 **TTL 抽头**；若走 232/485 收发器，电平与接法完全不同（485 需 A/B 差分）。

> **UART3 与 GMAC1 引脚互斥（踩过的坑）**：`gmac1_tx_bus2` 把 **GPIO3_B5** 复用成 `gmac1_txen`、`gmac1_clkinout` 把 **GPIO3_B6** 复用成 `gmac1_mclkinout`（func 1，`rk3588s-pinctrl.dtsi:446-471`），而这两个 pad 正是 `uart3m1` 的 TX/RX（func 10，同文件 `:3132-3138`）。
> 一旦 `&gmac1 { status = "okay"; }`，UART3 会表现为**完全静默**（pad 被 mux 走，8250 即使注册了 console 也发不出字符），且 pinctrl 释放不会回滚 mux。CM588 的两个 RJ45 实测是 PCIe 的 yt6801，gmac1 无用 → **`&gmac1` 必须保持 `disabled`**。
> 另外 gmac1 的 RGMII 组还占用 GPIO3_PA0/PA1/PB3/PB4，与板级 `open-gpio-pins` 重叠。

---

## 5. 各 UART 现状（kernel-6.1 + ztl 板级 DTS）

`ttySx` 编号来自 SoC dtsi 的 aliases（`rk3588s.dtsi:65-74`：`serial0 = &uart0` … `serial9 = &uart9`），所以 **uartN ↔ ttySN 一一对应**。

| UART | 设备节点 | 状态 | 来源 | 能否给命令行 |
|---|---|---|---|---|
| uart0 | `/dev/ttyS0` | okay（`uart0m2`） | `ztl-rk3588-cm.dts:542-545` | 需自加 getty |
| uart1 | — | disabled | `rk3588s.dtsi` | — |
| uart2 | `/dev/ttyFIQ0` | 被 fiq-debugger 占用（uart2 本身 disabled） | `rk3588-linux.dtsi:42-53` | ⚠️ 驱动层可用，但**本板排针未引出**，实际看不到输出 |
| uart3 | `/dev/ttyS3` | okay（默认组 `uart3m1`）；前提是 `gmac1` 保持 disabled | `ztl-rk3588-cm.dts:538-540` | ✅ 唯一可用的调试口（TTL 抽头）；需 `0003` + **`0006`** + `0004` 三个补丁 |
| uart4/5/6 | — | disabled | `rk3588s.dtsi` | — |
| uart7 | `/dev/ttyS7` | okay（默认组 `uart7m1`） | `ztl-rk3588-cm.dts:547-549` | 需自加 getty |
| uart8 | `/dev/ttyS8` | okay（`uart8m1` + ctsn） | `ztl-rk3588-cm.dts:275-279` | 蓝牙占用，不建议 |
| uart9 | — | 板级显式 disabled（evb7 dtsi 里是 okay） | `ztl-rk3588-cm.dts:708-710` | — |

> ⚠️ **改 DTS 的 `chosen/bootargs` 不足以让 ttyS3 变成 console**（2026-10-29 实测）：
> Rockchip u-boot 的 `board_fdt_chosen_bootargs()` 会往 env `bootargs` 里追加 `androidboot.*`，
> 随后 `fdt_chosen()` 用 `fdt_setprop()` **整体替换** DTB 的 `chosen/bootargs`，`console=` 全部丢失，
> 内核因此不注册任何 console、串口完全静默。必须同时应用
> `patches/rk3588/0006-rk3588-uboot-keep-ttyS3-console.patch`（u-boot 侧追加
> `console=ttyFIQ0 console=ttyS3,115200 earlycon=…0xfeb60000`）。
> 详见 `docs/dev/RK3588_TTYS3_CONSOLE_ENABLE.md` §10.4。

---

## 6. 开机可拿到命令行的通道清单

| 通道 | 设备 | 现在能否交互 | 需要改什么 | 限制 |
|---|---|---|---|---|
| **UART2 调试口** | `/dev/ttyFIQ0` | ✅ 内核日志 + **root shell**（`/dev/console` 就是它，兜底口） | 已就绪（cmdline 把 `console=ttyFIQ0` 放最后） | 固定 1500000；fiq-debugger 独占 uart2 |
| UART3 | `/dev/ttyS3` | ✅ 内核日志 + **独立 root shell**（inittab `ttyS3::respawn:-/bin/sh`） | 已由 `0003`（cmdline）+ `0004`（inittab）落地 | 115200；**前提是 `&gmac1` 保持 disabled**；接线需确认是 TTL 抽头 |
| UART0 / UART7 | `/dev/ttyS0` / `/dev/ttyS7` | ❌ | 同上 | 需确认板上是否引出 |
| HDMI 文本终端 | `/dev/tty1` | ❌ buildroot 无 getty | rootfs inittab 加 `tty1::respawn:/sbin/getty -L tty1 0 vt100` | 依赖显示器/键鼠（buildroot 无桌面） |
| SSH | — | 依赖 IP | — | 正是静态 IP 失效时不可用的通道 |
| U-Boot 提示符 | 同 UART2 | ❌ 无法中断 | — | `u-boot/.config:340` `CONFIG_BOOTDELAY=0` |

结论：**静态 IP 失效时的兜底方案就是 UART2 上的 `ttyFIQ0`**——它不依赖网络、不依赖桌面、不需要登录口令。

---

## 7. buildroot 与 debian 的差异

| 项 | buildroot | debian（bookworm 桌面） |
|---|---|---|
| init | busybox init，读 `/etc/inittab` | systemd（`/sbin/init → /lib/systemd/systemd`） |
| 串口交互 | `::respawn:-/bin/sh`（`BR2_TARGET_SERIAL_SHELL_SH=y`，`buildroot/package/busybox/busybox.mk:255`、`:266`）→ console 上 **root shell，无口令** | `getty.target.wants` 只有 `getty@tty1.service`；`serial-getty@.service` 虽被 `debian/mk-rootfs-bookworm.sh:74` 改成免密 root bash，但**没有任何实例被 enable** → 串口无 shell |
| 桌面 | 有 weston 组件，但无 display-manager，`tty1` 也无 getty → **串口是唯一本地交互口** | XFCE + lightdm 自动登录 |

即：**串口调试完全不依赖桌面**；相反，buildroot 因为"没有桌面"，才把交互入口放在串口上。

---

## 8. USB-TTL 适配器注意事项

- **必须支持 1500000 baud**：CH340 / CP2102 / FT232 可以；很多 PL2303 上限 1.2 Mbaud，接上去只会乱码。
- 接线：适配器 GND ↔ 板 GND（必须共地）、适配器 TXD ↔ 板 RX（GPIO0_B6）、适配器 RXD ↔ 板 TX（GPIO0_B5）；**不要接 3.3V**。
- 终端设置：`1500000 8N1`，无流控（fiq-debugger 不启 RTS/CTS）。

---

## 9. 板端自检命令

```sh
cat /proc/cmdline                 # 期望含 console=ttyFIQ0（earlycon 也应指向 0xfeb50000）
cat /proc/consoles                # 列出已注册 console 及其 tty
cat /sys/class/tty/console/active # 当前 /dev/console 落在哪个 tty
ls -l /dev/ttyFIQ0 /dev/ttyS*     # 哪些串口设备真的存在
dmesg | grep -i "fiq\|ttyS"       # 期望 "Registered fiq debugger ttyFIQ0"
ip -o link | grep -v "lo\|docker" # 静态 IP 失效时先看真实网口名
```

---

## 10. 与官方《UART 开发使用指南》的关系

官方指南描述的是**普通串口**路径：在 DTS 打开对应 uart 节点 + 配 pinctrl → 出现 `/dev/ttySx` → `echo`/`cat` 收发。这套机制在本板**同样成立**，本板 uart0/uart3/uart7 都已经 `okay`，节点已存在（见 §5）。

差异只在两点：

1. **调试口是例外**：UART2 由 `fiq-debugger` 接管，所以它的设备名是 `ttyFIQ0` 而非 `ttyS2`，并且作为内核 console 使用。
2. **指南只讲"串口通不通"，没讲"能不能拿到 shell"**：`/dev/ttySx` 存在 ≠ 有命令行，后者还需要 console/getty 配置。

---

## 11. 未覆盖 / 风险

| 项 | 说明 |
|---|---|
| 板上实际引出情况 | DTS 只说引脚复用，不等于排针已引出；UART0/UART3/UART7 的物理位置需对照 CM588 载板原理图与丝印 |
| UART3 电气形态 | 注释为 ttl/232/485 复合口，需确认抽头与收发器方向控制 |
| fiq-debugger 的独占性 | 想用 `ttyS2` 必须同时禁用 fiq-debugger 并改 cmdline，见 `docs/dev/RK3588_TTYS3_CONSOLE_ENABLE.md` §6 |
| 无 FIQ 级调试能力 | 普通 8250 console 在极端场景（中断上下文长时间卡死）不如 fiq-debugger，见同文档 §8 |
| `gmac1` 与 `uart3` 引脚互斥 | 启用 gmac1 会让 ttyS3 彻底静默（GPIO3_B5/B6 被 mux 成 GMAC），同时 `ethernetN` 别名会让 gmac 抢占 eth0/eth1，详见 §4 的互斥说明与 `docs/dev/RK3588_TTYS3_CONSOLE_ENABLE.md` §10.1 |
