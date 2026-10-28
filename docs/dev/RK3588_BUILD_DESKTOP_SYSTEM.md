# RK3588 编译桌面系统以便手动调试网络（Debian bookworm desktop）

> 背景：之前给 buildroot rootfs 固化静态 IP（`S45static-eth0`）后，烧录仍 "ip 不可达"，无法远程验证。
> 本文分析如何编译一个**带桌面的系统**（接 HDMI + USB 键鼠即可本地登录操作），用于在板端手动定位
> "eth0 为什么拿不到静态地址" 的根因。
>
> 前置知识：`docs/dev/RK3588_NETWORK_CONFIG_AND_ETH0_STATIC_IP.md`。
> 之前的 buildroot 网络修改（提交 `d61788fc1`：`S45static-net` + `interfaces.d/eth0|eth1` + `dhcpcd.conf`）
> **保留不动**，用于无桌面/量产场景；桌面系统仅用于**调试根因**，两者不冲突。
>
> **状态（2026-10-28）**：根因已定位，是两条叠加——
> ① busybox 无 `ifupdown` 小程序，`S40network` 应用不了 `interfaces.d/*`；
> ② **网口名不可靠**：GMAC 按 `ethernetN` 别名抢占 `eth0/eth1`，把真正的 PCIe `yt6801` RJ45 挤到
> `eth2/eth3`，"配 eth0"就配到了未插线的口。
> 现有方案 `S45static-net` 同时绕开这两点（busybox `ip` + 驱动/BDF 绑定），见
> `RK3588_NETWORK_CONFIG_AND_ETH0_STATIC_IP.md` §2.2 与 `RK3588_TTYS3_CONSOLE_ENABLE.md` §10.1。
> 本文其余内容作为当时的排查记录保留。

---

## 1. 为什么走 Debian 桌面，而不是继续用 buildroot

- buildroot 是精简嵌入式 rootfs，没有图形桌面；要在其上做 "手动操作" 只能靠串口/SSH，而当前正是
  SSH 连不上的状态。
- 本 BSP 自带完整的 Debian 构建体系（`debian/` 目录），提供 **bookworm desktop**（arm64/armhf），
  内含 XFCE/GNOME/LXDE/UKUI 等多种桌面，可直接接显示器 + 键鼠登录，开终端手动 `ip`/`nmcli`/`journalctl`
  排查网络，体验远好于 buildroot。
- buildroot 里虽有 `matchbox-desktop`、`weston` 等包，但只是轻量窗口管理器，不适合 "手动调试"
  （详见 §7 可选方案）。

结论：**上桌面 → 用 Debian bookworm desktop arm64**。

---

## 2. 构建机（devs，x86_64）前置条件

`device/rockchip/common/scripts/check-debian.sh` 会逐项校验，主要依赖：

1. **跨架构 chroot 支持**
   ```bash
   sudo apt-get install -y binfmt-support qemu-user-static
   # check-debian.sh 要求 qemu-aarch64-static 可用，推荐 qemu-8.0
   ```
2. **live-build / debootstrap（需支持 bookworm）**
   - `debian/ubuntu-build-service/packages/` 已带两个 deb，可先试：
     ```bash
     sudo dpkg -i debian/ubuntu-build-service/packages/*.deb
     sudo apt-get install -f
     ```
   - 若 check-debian.sh 报版本不支持，按提示换源码装：
     ```bash
     sudo apt-get remove live-build
     git clone https://salsa.debian.org/live-team/live-build.git --depth 1 -b debian/1%20230131
     cd live-build && sudo make install -j8
     sudo apt-get remove debootstrap
     git clone https://salsa.debian.org/installer-team/debootstrap.git --depth 1 -b debian/1.0.123+deb11u2
     cd debootstrap && sudo make install -j8
     ```
3. **e2fsprogs（mke2fs 需支持 `-d`）**
   ```bash
   # 若 check-debian.sh 提示 mke2fs 太旧：
   device/rockchip/common/scripts/install-e2fsprogs.sh
   ```
4. **构建机文件系统允许创建设备节点**
   - 若 debootstrap 报 `noexec or nodev`：
     ```bash
     sudo mount -o remount,dev "$(findmnt -fnu -o TARGET -T device/rockchip/common/scripts/check-debian.sh)"
     ```
5. **网络（最大风险点）**
   - `mk-base-debian.sh` 用 `live-build` 从镜像 debootstrap；`mk-rootfs-bookworm.sh` 在 chroot 里 `apt-get`
     装桌面 + RGA/MPP/gstreamer 等，需访问 Debian 镜像（默认 `mirrors.ustc.edu.cn/debian`）。
   - **构建机必须有可达的外网/镜像**，否则构建失败。可用环境变量切换镜像：
     ```bash
     export RK_DEBIAN_MIRROR=<你的镜像host，如 mirrors.tuna.tsinghua.edu.cn/debian>
     ```
   - `check-debian.sh` 会校验该镜像可达（走 `check-network.sh`）。

---

## 3. 构建流程

### 方式 A（推荐）：走 `build.sh` 统一编排

```bash
cd /data/kkkqkx/rk3588bsp

# 1) 把板级配置切到 debian rootfs 系统
#    （当前 rockchip_rk3588_ztl_cm588_defconfig 没有 RK_ROOTFS_SYSTEM，
#     这一步会在 .config 写入 RK_ROOTFS_SYSTEM="debian"）
./build.sh init debian

# 2) 构建 debian rootfs（内部：mk-base-debian.sh[TARGET=desktop ARCH=arm64]
#    -> mk-rootfs-bookworm.sh -> chroot apt-get 装桌面与 rockchip 包）
./build.sh debian          # 等价于 ./build.sh rootfs（rootfs 系统已设为 debian）

# 3) 打包成可烧录镜像
./build.sh updateimg
```

- 产物：`output/update/Image/update.img`（与之前路径一致）。
- `build_debian()` 会先检查 `linaro-bookworm-alip-*.tar.gz`，不存在就自动跑
  `mk-base-debian.sh` 生成基础桌面 tarball，再 `mk-rootfs-bookworm.sh` 生成 `binary/`，
  最后 `mk-image.sh` 产出 `linaro-rootfs.img` 并纳入固件打包。
- 分区表里 rootfs 为 `rootfs:grow`，会自动占满剩余 eMMC 空间，**无需手动改 parameter.txt**。

### 方式 B（手动，按 `debian/readme.md`）

```bash
cd /data/kkkqkx/rk3588bsp/debian
RELEASE=bookworm TARGET=desktop ARCH=arm64 ./mk-base-debian.sh   # 生成基础桌面 tarball
RELEASE=bookworm ARCH=arm64           ./mk-rootfs.sh             # 装 rockchip 包 + 桌面
./mk-image.sh                                                     # 产出 linaro-rootfs.img
# 再把 linaro-rootfs.img 当作 rootfs.img 喂给 ./build.sh updateimg
```

---

## 4. 烧录与启动

- 烧录同前（`rkflash.sh` / `upgrade_tool`），务必确认烧的是**新** `update.img`，
  且内核为 `6.1.118 (itc@itc)`（排除旧 5.10 未真正刷入的老问题）。
- Debian rootfs 比 buildroot 大很多（数 GB），烧录/首次启动更慢，启动时会做文件系统扩展（grow）。
- 接 **HDMI 显示器 + USB 键盘鼠标**，上电启动。

---

## 5. 登录与手动操作

- 默认用户来自 live-build `bootappend-live username=linaro`：`linaro` / `linaro`
  （部分镜像也可能直接 root 登录，视具体定制而定；桌面登录管理器会给出提示）。
- 打开桌面终端，即可手动调试网络——这正是当前 "ip 不可达" 问题的排查入口。

---

## 6. 在桌面上手动验证 "之前静态 IP 为什么失效"

> **关键认知**：buildroot 的网络补丁（`S45static-net`、`interfaces.d/eth0|eth1`、`dhcpcd.conf`）
> 是 **buildroot fs-overlay**，只作用于 buildroot 构建出的 rootfs。**Debian rootfs 里没有这些文件**。
> 所以在 Debian 上你验证的是**根因本身**，而不是那条补丁。

在桌面终端里逐步确认：

```bash
# 1) 网卡是否存在、是否 up、有没有地址
ip link
ip addr show eth0

# 2) 网络由谁管理（Debian 桌面用 NetworkManager / systemd-networkd，不是 ifupdown）
nmcli device status
journalctl -u NetworkManager --no-pager | tail -40

# 3) 直测静态地址/路由能否生效（绕过任何网络管理器）
#    先确认哪个口是 RJ45：本板两个 RJ45 是 PCIe yt6801，名字不一定叫 eth0
ip -o link; ls -l /sys/class/net/*/device/driver
sudo ip link set eth0 up                  # 换成上一步查到的 yt6801 口名
sudo ip addr add 192.168.80.202/24 dev eth0
sudo ip route add default via 192.168.80.254 dev eth0
ping -I eth0 192.168.80.254

# 4) 若想固化（仅调试用），可走 NetworkManager：
sudo nmcli con add type ethernet ifname eth0 ip4 192.168.80.202/24 gw4 192.168.80.254
# 或写 netplan：/etc/netplan/01-static.yaml 后 `sudo netplan apply`
```

借此可现场回答三点：
- **eth0 到底是不是那颗 RJ45 口**？→ 已答：**不是**。两个 RJ45 是 PCIe `yt6801`，GMAC 若 probe
  成功会先占 `eth0/eth1`，把 RJ45 挤到 `eth2/eth3`（详见 `RK3588_TTYS3_CONSOLE_ENABLE.md` §10.1）。
- **之前 buildroot 下为什么静态 IP 没生效**？→ 已答：两条叠加。① 本 BSP busybox 没有 `ifupdown`
  小程序，`S40network` 的 `ifup -a` 报 "applet not found"，`interfaces.d/*` 从不被执行；
  ② `denyinterfaces eth0` + 口名错位，导致真正插线的口既没 DHCP 也没静态地址。
- **镜像/内核是否真的刷进去了**？（桌面能起来本身就证明烧录成功；再 `uname -a` 核对 6.1.118）

---

## 7. 内核 / 显示前置（已确认基本具备，几乎不用改）

`kernel-6.1/arch/arm64/configs/ztl_defconfig` 已开启：

| 需求 | 配置项 | 状态 |
|---|---|---|
| 显示/HDMI | `CONFIG_DRM_ROCKCHIP=y`, `CONFIG_ROCKCHIP_DW_HDMI=y`, `CONFIG_DRM_PANEL_SIMPLE=y`, `CONFIG_ROCKCHIP_CDN_DP=y` | ✅ |
| GPU | `CONFIG_MALI_*=y`（Valhall/Midgard） | ✅（桌面可用软件渲染 llvmpipe 兜底，mali 为加速加分项） |
| 输入 | `CONFIG_INPUT_EVDEV=y`, `CONFIG_USB_HIDDEV=y` | ✅（建议再确认 `CONFIG_HID=y` / `CONFIG_USB_HID=y` / `CONFIG_HID_GENERIC=y` 已开，USB 键鼠依赖） |

若 `CONFIG_USB_HID`/`CONFIG_HID_GENERIC` 未开，补上后：
```bash
./build.sh kernel && ./build.sh updateimg
```

---

## 8. 保留 buildroot 网络修改的意义

- 提交 `d61788fc1` 的 buildroot 网络补丁**保留**，用于无桌面/量产镜像的静态 IP 固化
  （现为 `S45static-net`：busybox `ip` 小程序 + 按驱动/BDF 绑定双口，已编入 `update.img`）。
- 桌面镜像用于**调试根因**；两者独立：用 Debian 镜像定位问题，用 buildroot 镜像（含补丁）做最终固化。
- 若希望桌面镜像也固化静态 IP，应在 Debian 侧用 netplan / `nmcli` 等价配置（不在本补丁范围）。

---

## 9. 风险与耗时

- **最大风险是网络**：Debian 构建要拉大量包，镜像不可达必失败 → 用 `RK_DEBIAN_MIRROR` 换源。
- 首次构建耗时明显长于 buildroot（debootstrap + apt 装整个桌面 + RGA/MPP/gstreamer 等）。
- 镜像体积大，注意 eMMC 容量与烧录时间；rootfs 分区 `grow` 会自动扩容。

---

## 10. 可选：在 buildroot 内加轻量桌面（Weston / Matchbox）

若不想构建完整 Debian，只想有个本地终端/窗口来调试，可在 buildroot defconfig 开启：

```bash
# buildroot 配置里（或 make menuconfig）打开：
BR2_PACKAGE_WESTON=y        # 或 BR2_PACKAGE_XORG + 桌面环境
BR2_PACKAGE_MATCHBOX=y      # 轻量窗口管理器（仓库已有 matchbox-desktop 包）
```

然后 `./build.sh rootfs && ./build.sh updateimg` 重建。优点：快、能复用现有 buildroot 网络补丁（同一 rootfs）；
缺点：不是完整桌面，手动调试体验弱于 Debian。

---

## 附：buildroot 手动调试（串口）与"HDMI 黑屏"实测分析

### A. buildroot 可以手动调试吗？——可以，走串口 UART（无需桌面）

- 内核 bootargs（`rk3588-linux.dtsi`）：
  `earlycon=uart8250,mmio32,0xfeb50000 console=ttyFIQ0`。
  `0xfeb50000` = **UART2（RK3588 调试串口）**；`fiq_debugger` 的 `rockchip,baudrate = <1500000>`（注释：只支持 115200 / 1500000）。
- rootfs `/etc/inittab`（buildroot 默认）：`::respawn:-/bin/sh`——在 `/dev/console`（= `ttyFIQ0`）上
  **直接起 root shell（无密码）**。
- **操作**：USB-TTL（3.3V）接调试串口 TX/RX/GND，串口工具设 **1500000 8N1**，上电即可看到内核日志并拿到 `#`。
  若先进入 FIQ debugger，回车/`h` 后回车进入 shell。
- 注意：`/etc/inittab` **没有 tty1 的 getty**，且内核**没有 framebuffer 文本控制台**（见 B），
  所以 **HDMI 上不会有内核日志/登录提示**。rootfs 里另有 `S50dropbear`（SSH），但需网络先通。

### B. HDMI 黑屏是不是 buildroot 产物的问题？——不是"产物损坏"，是端口 + 控制台配置

实测依据（`kernel-6.1/.config` 与反编译 `ztl-rk3588-cm.dtb`）：
- 内核有 `CONFIG_DRM_ROCKCHIP=y`、`CONFIG_ROCKCHIP_VOP2=y`、`CONFIG_ROCKCHIP_DW_HDMI=y`；
- 但 **`CONFIG_FB`、`CONFIG_FRAMEBUFFER_CONSOLE`、`CONFIG_DRM_FBDEV_EMULATION` 在最终 `.config` 中都不存在**
  （内核是 `make ztl_defconfig rk3588.config rk3588_linux.config ztl_rk3588.config` 合并，fragment 把这些关了）；
- DTB：`hdmi@fde80000`（`rockchip,rk3588-dw-hdmi`）`status = "okay"`，`route-hdmi0` 也是 `okay`。
  → **HDMI 输出路径在产物里是启用的**，不存在"HDMI 被编译掉"的产物问题。

黑屏真实原因（按可能性）：
1. **没有文本控制台**：`console=ttyFIQ0`、无 tty1 getty、内核无 fbcon → 即使 HDMI 链路正常，
   也不会出现内核日志/登录提示，屏是黑的。
2. **显示端口不是 HDMI 输出**：CM588 DTS 里显示输出用的是 `&dp1`（注释 "cm588 dp part, use dp convert to vga"），
   且 `&route_dp1` 被 **disabled**（注释 "if open route_dp1, test screen will show blurred screen"）；
   同时 `&hdmirx_ctrler`（**HDMI 输入/采集**）是启用的。→ 板子的"HDMI"口很可能是**输入口**，
   接显示器当输出用必然黑屏。
3. 显示程序是 `S49weston`（Weston/Wayland，走 DRM），无对应连接器/权限时 weston 起不来，屏也黑
   （见 `/var/log/weston.log`）。

结论：**不是之前 buildroot 构建产物损坏**；是"HDMI 口用途 + 无 framebuffer 控制台"的问题。
若确要 HDMI 文本控制台：重编内核加 `CONFIG_FB=y` + `CONFIG_FRAMEBUFFER_CONSOLE=y`，
并在 inittab 加 `tty1::respawn:/sbin/getty -L tty1 0 vt100`（且先确认板子确有 HDMI **输出**口，否则用 DP）。

### C. 网络其他问题核查（本节结论已被 2026-10-28 实测推翻，见文末提示）

- 内核：`STMMAC_ETH=y`、`DWMAC_ROCKCHIP=y`、`PHYLIB=y`、`REALTEK_PHY=y`、`MDIO_DEVICE=y`
  → GMAC 与 RTL8211F PHY 驱动齐全（**但本板两个 RJ45 实测是 PCIe `yt6801`，不走 GMAC**）。
- DTS：`&gmac0` okay（`rgmii-rxid`，mdio0 `phy@1` rtl8211f，reset `gpio4 PB3`）；`&gmac1` **disabled**。
- `rcS` 是 `for ... $i start`（失败继续）→ `S40network`（`ifup` 缺失）失败**不会中断**。
- `/usr/sbin/ip`（iproute2）存在，依赖 `libcap.so.2`/`libc` 齐全。

> ⚠️ **本节原结论已作废**（原文：`eth0 = gmac0 = RJ45，接口名正确`、`S45static-eth0 会执行`）。
> 实测：SoC dtsi 的 `aliases { ethernet0 = &gmac0; ethernet1 = &gmac1; }` 会让任何 probe 成功的
> GMAC 抢占 `eth0/eth1`，把真正的 RJ45 挤到 `eth2/eth3`；脚本也已改名为 `S45static-net` 并按
> 驱动(`yt6801`)+PCI BDF 绑定。当前方案见 `docs/dev/RK3588_NETWORK_CONFIG_AND_ETH0_STATIC_IP.md` §2.2。

### D. 串口登录后逐项确认（定位"ip 不可达"）

```bash
uname -a                                   # 确认 6.1.118
cat /etc/init.d/S45static-net              # 确认补丁在镜像里（旧镜像里是 S45static-eth0）
ip -o link                                 # 两个口各自的名字，别假设 eth0
ls -l /sys/class/net/*/device/driver       # 驱动应为 yt6801
grep S45static-net /dev/kmsg               # 脚本实际配给了谁（每口一行）
ip -br addr show                           # 是否有 192.168.80.202/24 与 .201/24
ip route                                   # 默认路由
dmesg | grep -iE 'eth|yt6801|gmac|stmmac|phy'   # 链路是否 up / 报错
cat /var/log/weston.log 2>/dev/null        # 显示是否 weston 启动失败
```
