# EN588 新内核 SDK 补丁适用性分析

基线：`devs:/data/kkkqkx/EN588-linux-6.1`（`de7b21fb0`，两棵树均为 6.1.118）。
结论总览：旧补丁**没有可直接 `git am` 的**，但意图大多仍然有效，需 rebase 或重做。

## patches/rk3588（旧 6.1 树补丁）

| 补丁 | 结论 | 理由 |
|---|---|---|
| 0001 内核 AF_XDP/eBPF（ztl_defconfig + Config.in.kernel + 关 STMMAC_UIO） | 需 rebase，不可直用 | 新 SDK 无 `ztl_defconfig`；`Config.in.kernel` 是上游原样，hunk 上下文或可命中但缺 fragment 含义；`rockchip_linux_defconfig` 本来就没有 XDP/BPF/BTF/HUGETLB（使能意图仍成立），也没有 `STMMAC_UIO`（第二提交已过时）。动工前先确认构建走 `kernel/` 还是 `kernel-6.1/`（`Config.in.kernel` 的 DTS 目录默认指向 `kernel/`）。 |
| 0002 buildroot fs-overlay 双网口静态 IP | 暂缓 | 文件全是新增，技术上能合入；但标准 `rockchip_rk3588_defconfig` 未引用该 overlay（只有 ipc 版引用了 `fs-overlay-ipc`），不接线就不进镜像。且 EN588 产品走 Debian/GNOME 镜像，buildroot 这条路可能根本用不上，先定 rootfs 路线再动手。 |
| 0003 ttyS3 控制台（DTS chosen + inittab + u-boot） | 拆分处理 | DTS 意图成立，但目标要换成 `ztl-rk3588-en.dts`（en 的 uart3/uart2/gmac1 已配好，只缺 `chosen`）；inittab 部分失效（新 SDK buildroot 无 inittab）；u-boot 部分原理成立（同名函数同样覆盖语义），按新行号重做。 |
| 0004 debian bookworm locale/root/ssh（+ chroot bash） | 最可能直接适用，先做 `git apply --check` | `mk-rootfs-bookworm.sh` 同名存在，第 51 行裸 `chroot` 原样未修；locale/ssh/password 后半段改的是同一文件。 |
| 0005 debian 双网口静态 IP | 大概率可应用，需 `--check` + 上板验证 | 全是 overlay 新增文件，无文件名冲突；overlay 主 NM 配置（`managed=false`）与新增的 unmanaged drop-in / static-net service 是否语义一致，上板用 `nmcli` 确认。 |

## patches/rk3588-5.10

**不适用。** 新 SDK 没有 5.10 内核；yt6801 在两棵 6.1 树里都已在树内且
`CONFIG_YT6801=y`，移植补丁无存在意义；bullseye 旧流程被 bookworm
和 ubuntu22.04 GNOME 预制镜像取代。

## patches/dpdk、ice_drv、iavf_drv

不受影响。这些是 DPDK 版本补丁，与内核 SDK 无关。

## 建议顺序

1. 对 0004、0005 做 `git apply --check`（devs 工作区上），能过的先合入 `patches/en588/`。
2. 定 rootfs 路线（Debian bookworm 自编 vs ubuntu GNOME 预制镜像 vs buildroot），再决定 0002 的去留。
3. 重做 0001（先定 `kernel/` 与 `kernel-6.1/` 用哪棵）和 0003（en DTS chosen + u-boot）。
4. 5.10 补丁封存，不再跟进。
