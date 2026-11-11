# EN588 SDK 基本信息

来源：`devs:/data/kkkqkx/EN588-oepnsource-SDK/`（目录名拼写原文如此）。
解压后的工作区：`devs:/data/kkkqkx/EN588-linux-6.1`。

## 包内容

| 文件 | 大小 | 内容 |
|---|---|---|
| `EN588-linux-6.1-Opensource.tgz` | 约 7.6G | 内核 SDK（含 `.git`），单独提交 `de7b21fb0 rk3588-6.1 Open Source Project` |
| `ubuntu22.04-gnome-opensource.tgz` | 约 2.4G | 单个预制 rootfs 镜像 `ubuntu22.04-3588-gnome-open-20261102-173246.img`，无 patch 流程 |
| 两个 `.md5` 文件 | | 与上表同名，校验包完整性用 |

注意：`EN588-linux-6.1-Opensource.tgz` 解包后只有 `.git/`，没有工作区文件，
必须在 devs 上 `git reset --hard HEAD`（约 227772 个文件，走 setsid 后台）才能得到源码树。
远端 git origin 为内网地址（`http://192.168.101.29:7990/scm/opk/rk3588-6.1-linux.git`），本地不可达；
改动一律以本地 `patches/en588/` 为真相源，不指望远端 git 同步。

## 源码树布局（标准 Rockchip Linux SDK 版型）

顶层：`app buildroot build.sh common debian device docs external hal kernel kernel-6.1`
`Makefile prebuilts rkbin rkflash.sh rockdev rtos tools u-boot yocto`

- 双内核树：`kernel/` 与 `kernel-6.1/` **同为 6.1.118**
 （`VERSION=6 PATCHLEVEL=1 SUBLEVEL=118`），没有 5.10 内核。
- 板级：`device/rockchip/.chips/rk3588/` 下有
  `rockchip_rk3588_ztl_{ca,cm,en}588_defconfig`，每个只有两行
  （`RK_KERNEL_DTS_NAME="ztl-rk3588-<board>"` + `RK_USE_FIT_IMG=y`），其余走公共默认值。
- DTS：`ztl-rk3588-{ca,cm,en}.dts` 在两棵内核树里都存在。
  en588 已配好 `uart3 okay` / `uart2 disabled` / `gmac1 disabled`，
  但 en/cm 两个 DTS 都**没有 `chosen/bootargs`**。
- 内核配置：没有 `ztl_defconfig`；`rockchip_linux_defconfig`（599 个符号）
  已有 `CONFIG_YT6801=y`、`CONFIG_STMMAC_ETH=y`，
  但无 `XDP_SOCKETS` / `BPF_SYSCALL` / `BPF_JIT` / `DEBUG_INFO_BTF` / `HUGETLBFS`，
  也没有 `STMMAC_UIO`。
  `device/rockchip/common/configs/Config.in.kernel` 为上游原样
  （默认 `rockchip_linux_defconfig`，无 fragment 默认）。
- yt6801 驱动在两棵内核树里都已在树内，无需移植。
- buildroot：`board/rockchip/rk3588/fs-overlay` 只有 pulseaudio/udev，
  无 `etc/init.d`、无 inittab；标准 `rockchip_rk3588_defconfig`
  未引用该 overlay（只有 `ipc_defconfig` 引用 `fs-overlay-ipc`）。
- debian：bookworm（`mk-rootfs-bookworm.sh`），overlay 有
  `etc/NetworkManager/NetworkManager.conf`（`managed=false`），
  无 ztl 静态网络文件、无 `usr/local/sbin`；
  `mk-rootfs-bookworm.sh` 第 51 行仍是裸 `chroot $TARGET_ROOTFS_DIR`
  （缺 `/bin/bash`），默认 locale 为 `zh_CN.UTF-8`。
- u-boot：`u-boot/arch/arm/mach-rockchip/board.c` 的
  `board_fdt_chosen_bootargs()` 仍是 env `bootargs` 覆盖 DTB `chosen` 的语义。

## 与旧 rk3588 树的关系

旧 `rk3588bsp`（6.1.118）/ `rk3588_5.1_XDP`（5.10.226）的 bring-up 结论
（串口用 uart3、gmac1 必须 disabled、u-boot 会吞掉 DTB bootargs）
在本 SDK 的 en588 DTS 里已有部分体现（uart3/uart2/gmac1 配置），
但 `chosen`、XDP 内核配置、rootfs 网络配置三块仍是空白，正好对应旧补丁要补的内容。
旧补丁能否直接用见 `EN588_PATCH_APPLICABILITY.md`。
