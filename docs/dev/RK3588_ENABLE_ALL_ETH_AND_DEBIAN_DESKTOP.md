# RK3588：启用全部网口（gmac0/gmac1）并构建 Debian 桌面系统

> 关联文档：
> - `docs/dev/RK3588_NETWORK_CONFIG_AND_ETH0_STATIC_IP.md`（buildroot 静态 IP / busybox 缺 ifupdown）
> - `docs/dev/RK3588_BUILD_DESKTOP_SYSTEM.md`（桌面构建分析、串口/HDMI 黑屏结论）
>
> 本文记录 2026-10-27 的两处源码改动、对应 patch，以及 Debian 桌面系统的构建与产物。
> 基线提交：BSP 仓库 `d61788fc1`。

---

## 1. 变更概览

| 编号 | 文件 | 变更 | 目的 |
|---|---|---|---|
| 0003 | `kernel-6.1/arch/arm64/boot/dts/rockchip/ztl-rk3588-cm.dts` | `&gmac1` 由 `disabled` 改为 `okay` | 启用全部有线网口（gmac0 已 okay + gmac1） |
| 0004 | `debian/mk-rootfs-bookworm.sh` | `sudo chroot $TARGET_ROOTFS_DIR` → `... /bin/bash` | 修复 chroot 用宿主 `$SHELL`(zsh) 导致 exit 127 |

- 本地 patch：`patches/rk3588/0003-kernel-6.1-enable-gmac1-all-ethernet-ports.patch`、`patches/rk3588/0004-rk3588-debian-chroot-bash-fix.patch`
- 两者都是 `git diff` 风格的工作区补丁；BSP 为单一 git 仓库（`kernel-6.1` **不是**子模块）。

应用（在 BSP 根目录、干净基线上）：

```bash
git apply patches/rk3588/0003-kernel-6.1-enable-gmac1-all-ethernet-ports.patch
git apply patches/rk3588/0004-rk3588-debian-chroot-bash-fix.patch
# 或： patch -p1 < patches/rk3588/0003-...patch
```

---

## 2. 变更 0003：启用 gmac1（全部网口）

- CM DTS 中 `&gmac0` 原本就是 `okay`（`rgmii-rxid`、`mdio0 phy@1` rtl8211f、reset `gpio4 PB3`、`tx_delay=0x44`）。
- `&gmac1` 在文件末尾被覆盖成 `disabled`；改为 `okay` 后，继承 `rk3588-evb7-v11.dtsi` 的完整配置：
  `phy-mode="rgmii-rxid"`、`clock_in_out="output"`、`snps,reset-gpio=<&gpio3 RK_PB7 GPIO_ACTIVE_LOW>`、
  `pinctrl-0=<gmac1_miim,gmac1_tx_bus2,gmac1_rx_bus2,gmac1_rgmii_clk,gmac1_rgmii_bus>`、`tx_delay=0x43`、
  `phy-handle=<&rgmii_phy>`（即 `mdio1 phy@1`）。

编译验证（`dtc -I dtb -O dts kernel-6.1/arch/arm64/boot/dts/rockchip/ztl-rk3588-cm.dtb`）：

- `ethernet@fe1b0000`（gmac0）`status="okay"`；
- `ethernet@fe1c0000`（gmac1）`status="okay"`，`mdio1`/`rgmii_phy` 节点均存在。

> 注意：DTS 级有线口只有 gmac0/gmac1。若 CM588 硬件未引出第二颗 PHY，gmac1 会打印 `no phy`/无链路，但不致命；
> 若板上有 PCIe 网卡，由 PCIe 枚举自动识别，无需改 DTS。

---

## 3. 变更 0004：chroot 显式 /bin/bash

- 现象：`./build.sh debian` 在 `mk-rootfs-bookworm.sh` 的
  `cat << EOF | sudo chroot $TARGET_ROOTFS_DIR` 处失败：
  `chroot: failed to run command '/usr/bin/zsh': No such file or directory` → exit 127。
- 根因：`chroot` 未指定命令时使用 `$SHELL`；而 `sudo` 会把 `SHELL` 设为调用者在 `/etc/passwd` 里的 shell
  （`itc` 为 `/usr/bin/zsh`），目标 Debian rootfs 只有 `/bin/bash`、`/bin/sh`（dash）。
- 修复：chroot 显式指定 `/bin/bash`（目标 rootfs 内存在）。

> 该改动只影响构建机的 Debian 打包脚本，不影响运行期。

---

## 4. Debian 桌面构建

### 4.1 构建机前置（devs，x86_64）

- `binfmt-support`、`qemu-user-static`（`qemu-aarch64-static` 已注册 binfmt）；
- **新版** `live-build`、`debootstrap`：可用仓库自带 `debian/ubuntu-build-service/packages/*.deb` 安装
  （系统自带 `live-build 3.0a57`/`debootstrap 1.0.95` 不支持 bookworm）：
  ```bash
  sudo dpkg -i debian/ubuntu-build-service/packages/*.deb
  ```
- `mke2fs` 支持 `-d`；
- 可访问 Debian 镜像（默认 `mirrors.ustc.edu.cn`；可用 `RK_DEBIAN_MIRROR` 覆盖）。

### 4.2 命令

```bash
cd /data/kkkqkx/rk3588bsp
./build.sh kernel      # 编译含 gmac1 的新 DTB + FIT boot.img
./build.sh debian      # Debian bookworm arm64 desktop(XFCE) rootfs，并生成 rockdev/rootfs.img
./build.sh updateimg   # 打包 update.img
```

说明：`./build.sh debian` 的 `build_hook debian` 会以 `ROOTFS=debian` 构建，并在结束时把
`output/debian/images/rootfs.ext4` 软链为 `output/firmware/rootfs.img`，因此不依赖 `RK_ROOTFS_SYSTEM` 的值。

### 4.3 产物（devs）

| 产物 | 说明 |
|---|---|
| `output/update/Image/update.img`（约 4.8GB，头部 `RKFW`） | 可烧录固件；`output/firmware/update.img` 为其软链 |
| `output/firmware/rootfs.img` → `debian/linaro-rootfs.img`（4.65GB ext4） | Debian 12.15 bookworm，XFCE |
| `output/firmware/boot.img` → `kernel-6.1/boot.img` | 含 gmac1 的新 FIT |

桌面信息：

- Debian 12.15 bookworm + XFCE；`lightdm` 自动登录 `linaro`；
- 系统账户：`root`、`linaro`（`/home/linaro`）；
- 网络由 **NetworkManager** 管理（`/usr/bin/nmcli`、`nmtui`）。

---

## 5. 上板验证建议（桌面系统）

```bash
ip link                      # 期望 eth0、eth1（gmac0/gmac1；若第二口无 PHY 可能只有 eth0）
ip addr
nmcli device status
dmesg | grep -iE 'eth|gmac|stmmac|phy'
ethtool eth0 2>/dev/null | head
```

两个网口用 `nmcli` 配置（示例）：

```bash
sudo nmcli con add type ethernet ifname eth0 ip4 192.168.80.202/24 gw4 192.168.80.254
# eth1 同理
```

---

## 6. 注意事项

- **Debian rootfs 不含 buildroot 的 `S45static-eth0`**（那是 buildroot fs-overlay，见 `0002` 补丁）。
  桌面系统请用 NetworkManager 配置网口；buildroot 镜像仍可用 0002 的静态 IP 方案。
- `update.img` 约 4.8GB，烧录与首次启动较慢；rootfs 分区为 `grow`，会自动占满 eMMC。
- 构建期间曾临时启用免密 sudo，**完成后已清除**（`/etc/sudoers.d/99-itc-nopasswd` 已删除，`sudo -n` 需密码）。
- 构建残留（devs）：`debian/linaro-bookworm-alip-20261027-1.tar.gz`（基础 tarball）、`debian/linaro-rootfs.img`（4.65GB）。
  如需重打包可复用该 tarball，跳过基础桌面下载。

---

## 7. 复现清单（从零）

1. 应用 `0003`、`0004` 补丁；
2. 安装新版 `live-build`/`debootstrap`（见 §4.1）；
3. `./build.sh kernel` → `./build.sh debian` → `./build.sh updateimg`；
4. 烧录 `output/update/Image/update.img`，接 HDMI/DP + USB 键鼠，自动登录 `linaro` 进入 XFCE；
5. 用 `nmcli` 配置并验证两个网口。
