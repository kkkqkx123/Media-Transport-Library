# Patches

本目录下的 patch 是内核/驱动代码的真相源（rk3588 系列仓库不使用 git 全量同步）。
每个 patch 集合按编号顺序应用；一套 patch 可一起应用。

## rk3588 (kernel 6.1)
目录：`patches/rk3588/`

| 文件 | 内容 | 前置 |
|---|---|---|
| 0001-kernel-6.1-enable-af_xdp-ebpf-for-mtl.patch | 6.1 内核启用 AF_XDP/eBPF 以支持 MTL | base |
| 0002-rk3588-fs-overlay-eth-static-ip.patch | 文件系统 overlay + eth 静态 IP | 0001 |
| 0003-rk3588-ttyS3-console.patch | ttyS3 控制台使能 | 0002 |
| 0004-rk3588-debian-rootfs-locale-root-ssh.patch | Debian rootfs locale/root/ssh 配置 | 0003 |
| 0005-rk3588-debian-static-net.patch | Debian 静态网络配置 | 0004 |

（提交名+hash：远端 base commit 为初始提交；各 patch 对应的 commit hash 以 `git am` 应用后的 `git log` 为准，记录在对应 patch 的 `From` 行。）

## rk3588-5.10
目录：`patches/rk3588-5.10/`（若存在）：5.10 内核相关 patch，顺序同上。

## dpdk (21.11 / 23.11 / 25.11)
目录：`patches/dpdk/<ver>/`，各 patch 独立，按编号顺序应用。详见各子目录。

## ice_drv / iavf_drv
目录：`patches/ice_drv/`、`patches/iavf_drv/`，用于硬件速率限制 pacing，需配合 `script/build_drivers.sh`。
