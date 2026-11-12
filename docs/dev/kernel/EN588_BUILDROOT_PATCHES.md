# EN588 buildroot 补丁组（patches/en588）：systemd/ssh 现状与四补丁说明

## systemd / ssh 是否默认开启

| 项 | buildroot（本次编译目标） | Debian bookworm（本次不用，只作对照） |
|---|---|---|
| init | 否。默认 BusyBox init；`BR2_INIT_SYSTEMD` 未设置。systemd-networkd 的 `default y` 只在选了 systemd 后才生效。 | 是，Debian 原生 systemd。 |
| ssh | 否。dropbear 只在 `network.config` 里，而 `rockchip_rk3588_defconfig` 只引了 `network/chromium.config`，没引它；root 密码缺省是 `rockchip`（common.config）。 | 未确认默认装没装 openssh（overlay 里没看到 ssh 配置；bookworm 镜像未展开验证，不在本次范围）。 |

结论：buildroot 镜像要 systemd + ssh，必须显式开。这就是 0003。

## 四补丁（顺序即依赖顺序，以 rk3568-patch 为准）

| 补丁 | 内容 | 说明 |
|---|---|---|
| 0001 内核 | 芯片配置加 `RK_KERNEL_PREFERRED="6.1"`；`Config.in.kernel` 给 rk3588 家族加 `ztl_rk3588.config` fragment 默认；新建该 fragment（XDP/BPF/BTF/HUGETLB/UFS 精简增量） | 不照抄旧 rk3588 0001（它把全芯片默认 defconfig 换成 919 行拷贝，还缺 PREFERRED，新树 fresh 编译会掉进不存在的 5.10）。fragment 只放相对上游 defconfig 的差量。 |
| 0002 网络+dns | 新 buildroot defconfig `rockchip_rk3588_en588_defconfig`（include 原 rk3588 + 板级 overlay）；芯片配置加 `RK_BUILDROOT_BASE_CFG="rk3588_en588"`；overlay 带双栈配置：ifupdown（静态 .202 + dns + udhcpc 二地址脚本）+ systemd-networkd（10-eth0.network，Address/Gateway/DNS + DHCP=ipv4）+ resolv.conf | rk3568 `0001-IP-DHCP-IP` / `0002-network` 原样思想，IP 换成 .202。defconfig 独立，避免污染 evb。 |
| 0003 systemd+ssh | 同一 defconfig 追加 `BR2_INIT_SYSTEMD=y`、dropbear 三件套、root 密码 `root` | 照抄 rk3568 `qt_runtime.config` 做法。注意它改变 0002 的生效路径：systemd 下 interfaces 文件失效（无 S40network），eth0 由 networkd + 10-eth0.network 接管；去 0003 则走 interfaces + udhcpc。两套文件都在 0002 里，所以顺序无关、行为明确。 |
| 0004 ttyS3+adb | en DTS 加 chosen（uart3 最后）；u-boot `board_fdt_chosen_bootargs` 补 console（防 fdt_chosen 整段覆盖）；defconfig 加 `BR2_PACKAGE_ANDROID_ADBD`；overlay 补 usbdevice 全套（service + wants 软链 + S50usbdevice + .usb_config + udev 规则 + profile.d）；内核 fragment 追加 `CONFIG_USB_FUNCTIONFS=y`（原缺失，adbd 无法挂载 functionfs）；bootargs 追加 `net.ifnames=0 biosdevname=0`（保证网卡名仍为 eth0，匹配 10-eth0.network） | inittab 不带：systemd 从 console= 自动起 getty，旧 0003 的 inittab 在新树 overlay 未接线本来就是死的。usbdevice 脚本与 debian overlay / rk3568 rkscript 同源。 |

## 同步与编译（见 KERNEL_REPO_SYNC_GUIDE）

真相源 `patches/en588/`；远端 `/data/kkkqkx/en588-patches/` 中转；
工作区 `/data/kkkqkx/EN588-linux-6.1` 改前 `reset --hard de7b21fb0` 再 `git am`。
镜像编译在 devs：选板 → uboot → kernel → buildroot → firmware → updateimg，
全程 setsid + 日志落盘。
