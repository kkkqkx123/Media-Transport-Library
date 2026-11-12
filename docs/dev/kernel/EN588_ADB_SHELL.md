# EN588 通过 USB-OTG 用 adb shell 调试的可行性分析

结论：可行。EN588 SDK 树内已有与 rk3568 同源的全套 usbdevice gadget 框架，
且开机自启动已接好；唯一缺口是 `/usr/bin/adbd` 二进制两棵树里都没有，
补上即可。串口起不来时，接 USB-OTG 线到 PC 就能 `adb shell` 进终端。

## 现状对照（rk3568-sdk vs EN588-linux-6.1）

| 部件 | rk3568 | EN588 | 说明 |
|---|---|---|---|
| `debian/overlay/usr/bin/usbdevice`（configfs 组装 adb/functionfs gadget） | 有 | 有，逐字节相同 | 默认 `USB_FUNCS=adb`，VID 0x2207 |
| `debian/overlay/etc/profile.d/usbdevice.sh` / `adbd.sh` | 有 | 有，逐字节相同 | 默认只开 adb；`ADBD_SHELL=/bin/bash` |
| `usbdevice.service` + `sysinit.target.wants` 使能 | 有 | 有（wants 下是到 `/lib/systemd/system` 的软链） | 开机即起，无需串口干预 |
| 内核 gadget：`USB_DWC3`、`USB_GADGET`、`CONFIGFS`、`CONFIGFS_F_FS`、`ACM`、`MASS_STORAGE`、`F_UVC`、`TYPEC_*` | 全开 | 全开（`rockchip_linux_defconfig`，两棵 6.1 树一致） | 内核零改动 |
| DTS OTG | 各板自配 | en 继承 `rk3588-evb7-v11.dtsi`：`usbdrd_dwc3_0` 为 `dr_mode="otg"` + role-switch，`vbus5v0_typec` 等已覆盖 | 见下节硬件确认 |
| `/usr/bin/adbd` 二进制 | 缺（overlay-debug 下无 adb 目录） | 缺（同左） | 唯一缺口 |
| `external/rkscript/` + rkstage0 安装脚本 | 有（但 adbd 源同样缺失） | 无此目录；rkstage0 的 `03-sys-storage/00-run.sh` 在 bookworm 流程里无人调用 | overlay 路径才是实际生效的，不用管 rkstage0 |

## 缺口补法：adbd 二进制

三选一，推荐第一种：

1. Debian 包（最干净）：`mk-rootfs-bookworm.sh` 的 `APT_INSTALL` 里加
   `android-tools-adbd`。动工前在 devs 上用 bookworm 源确认包存在
   （`apt-cache policy` 走 `mirrors.ustc.edu.cn/debian` 的 bookworm）。
2. 预制二进制：aarch64 的静态 adbd 丢进 overlay（如 `debian/overlay/usr/bin/adbd`），
   随 `patches/en588/` 一起管。来源必须可信（历史产物或自编），不要来路不明的网盘二进制。
3. 自行编译 AOSP `system/core/adb`：最重，不推荐。

adbd 以 root 跑，正好符合调试用途；`adb shell` 进去就是 root。

## 硬件/DTS 确认（上板前必做）

- 先确认 EN588 板上 USB-OTG 口的物理形态：如果是 Type-C 且 CC/TCPC 按 evb7 接线，
  现有 DTS 不用动，插线即被识别为 device。
- 如果该口没有 TCPC/CC 接线（只有 DP/DM + VBUS/ID），role-switch 切不过去，
  gadget 起不来。退路是在 en DTS 里把 `&usbdrd_dwc3_0` 强制为 device 模式，
  即覆盖 `dr_mode = "peripheral"` 并删掉 `usb-role-switch`。
- 上板验证（有串口时一次即可，之后可纯 adb）：
  `ls /sys/class/udc` 非空，
  `cat /sys/class/android_usb/android0/state` 应为 `CONFIGURED`；
  PC 侧 `adb devices` 出现设备后 `adb shell`。

## 与其他工作的关系

- 与 MTL 的 XDP 内核配置（`patches/en588/` 未来的 0001）无冲突：
  加 fragment/defconfig 时原样保留上面这组 USB gadget 选项即可。
- 与串口补丁是互补关系：adb 不依赖 console，`chosen` 修坏了也不影响；
  但 adb 起来要求系统能进到 systemd，所以它替代不了 loader/u-boot 阶段的调试。
- Windows 主机需要装 Rockchip USB 驱动，Linux 主机开箱即用。
