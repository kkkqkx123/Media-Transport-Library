# EN588：Ubuntu 整机镜像的最小内核替换

## 1. 需求

目标是在 Ubuntu 原始整机镜像上运行 MTL，同时尽量保留原始镜像的内部结构。

硬需求：

- Ubuntu 原始镜像可启动、可使用 adb/network/ssh。
- MTL 需要 DPDK，因此内核必须支持 `CONFIG_HUGETLBFS`。
- MTL 使用 generic XDP 时，内核至少需要 `CONFIG_XDP_SOCKETS` 和 `CONFIG_BPF_SYSCALL`。
- PTP 为单向软件方案，不依赖内核 PHC，不需要修改镜像。

非需求：

- 不需要迁移 buildroot 的 0002/0003/0004。
- 不需要重新打包 rootfs。
- 不需要 BPF JIT、XDP_SOCKETS_DIAG、BTF，除非后续改用 native/driver XDP 或 CO-RE 程序。

## 2. 内核修改

在 `kernel-6.1/arch/arm64/configs/ztl_rk3588.config` 中保留：

```text
CONFIG_XDP_SOCKETS=y
CONFIG_XDP_SOCKETS_DIAG=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_BPF_JIT_ALWAYS_ON=y
CONFIG_BPF_UNPRIV_DEFAULT_OFF=y
CONFIG_BPF_STREAM_PARSER=y
CONFIG_CGROUP_BPF=y
CONFIG_HUGETLBFS=y
CONFIG_DEBUG_INFO=y
CONFIG_DEBUG_INFO_BTF=y
```

其中对当前 generic XDP + MTL 运行真正必要的最小集合为：

- `CONFIG_XDP_SOCKETS=y`
- `CONFIG_BPF_SYSCALL=y`
- `CONFIG_HUGETLBFS=y`

其余 BPF/XDP 诊断或调试项可保留，但不是最小运行条件。

## 3. 子镜像来源确认

重新打包前，所有子镜像必须来自预编译 Ubuntu 整机包：

- `MiniLoaderAll.bin`
- `parameter.txt`
- `uboot.img`
- `misc.img`
- `boot.img`（仅作为被替换对象）
- `recovery.img`
- `userdata.img`
- `oem.img`
- `rootfs.img`

禁止混用 SDK buildroot 产物作为 rootfs/oem/userdata/recovery/misc/uboot。

## 4. 最小替换流程

1. 解包预编译 Ubuntu 整机镜像。
2. 保留所有预编译子镜像。
3. 仅将 `boot.img` 替换为当前内核构建产物。
4. 使用相同 `package-file` 重新打包。
5. 使用 `rkImageMaker -RK3588` 生成最终 `update.img`。

## 5. 参考结果

- 内核构建：`KERNEL_EXIT=0`
- 打包：`AFP_EXIT=0`、`MAKER_EXIT=0`
- 产物：`/data/kkkqkx/pack-minimal/update.img`
- 仅 `boot.img` 来自当前内核构建，其余子镜像均来自预编译 Ubuntu 整机包
