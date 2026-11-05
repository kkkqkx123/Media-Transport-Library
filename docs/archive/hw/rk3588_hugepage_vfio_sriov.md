# RK3588 开发板 hugepage / VFIO / SR-IOV 支持情况

## 平台

- 板子：RK3588，内核 `5.10.226-g40733b7b2aa6-dirty`，aarch64
- 网卡：2 × Motorcomm yt6801（PCIe，`0002:21:00.0`、`0003:31:00.0`）
- 访问：`ssh board`

## 2MB hugepage

- 状态：**支持**
- 证据：
  - `CONFIG_HUGETLBFS=y`，`CONFIG_SYS_SUPPORTS_HUGETLBFS=y`
  - `/sys/kernel/mm/hugepages/hugepages-2048kB` 存在
  - `hugetlbfs` 已挂载到 `/dev/hugepages`
  - 实测写入 `vm.nr_hugepages=4` 后 `HugePages_Total: 4`（8MB），`HugePages_Free: 4`
- 结论：MTL 的 DPDK hugepage 需求可满足，2MB hugepage 可用。

## VFIO

- 状态：**不支持（当前内核未编译）**
- 证据：
  - `# CONFIG_VFIO is not set`
  - 无 `/dev/vfio/` 目录
  - 内核模块树中无 vfio 相关模块
  - `CONFIG_IOMMU_SUPPORT=y` 与 IOMMU groups 存在，但没有 VFIO 用户态通道
- 结论：当前内核无法使用 `vfio-pci` 绑定网卡，DPDK PMD 的 VFIO 路径不可用。

## SR-IOV VF

- 状态：**不支持（当前内核未编译，且网卡大概率不支持）**
- 证据：
  - `# CONFIG_PCI_IOV is not set`
  - 两个 yt6801 设备均无 `sriov_totalvfs` 文件
  - 未启用 `CONFIG_UIO_PCI_GENERIC`（仅 `CONFIG_UIO`、`CONFIG_STMMAC_UIO`）
- 结论：当前无法创建 SR-IOV VF；yt6801 驱动/硬件大概率也不提供 SR-IOV。

## 对 MTL 的影响

- `MTL_PMD_DPDK_USER` 的 PCI 直通路径在当前板子内核上不可用（无 VFIO/UIO PCI generic）。
- 可用替代后端：`MTL_PMD_NATIVE_AF_XDP`、`MTL_PMD_KERNEL_SOCKET`、`MTL_PMD_DPDK_AF_PACKET` 等。
- 若需要 DPDK PMD 物理网卡路径，需重编内核开启 `CONFIG_VFIO`、`CONFIG_VFIO_PCI`（必要时 `CONFIG_PCI_IOV`），并确认网卡/驱动支持。

## 编译服务器内核源码分析（`ssh devs`）

- 源码目录：`/data/kkkqkx/rk3588_5.1_XDP`（Rockchip SDK，内核 5.10，GKI/rockchip 配置）
- `.config` / `rockchip_linux_defconfig` / `ztl_defconfig` 中均未开启 `CONFIG_VFIO`、`CONFIG_VFIO_PCI`、`CONFIG_PCI_IOV`
- Kconfig 依赖检查：
  - `VFIO`：`menuconfig VFIO`，`select IOMMU_API`，`select VFIO_IOMMU_TYPE1 if MMU && (X86 || S390 || ARM || ARM64)`。ARM64 满足，软件上可开启。
  - `VFIO_PCI`：`depends on VFIO && PCI && EVENTFD` 且 `depends on MMU`。软件上可开启。
  - `VFIO_NOIOMMU` 可作为无 IOMMU 时的非安全回退，但会 taint 内核。
- 硬件/IOMMU 拓扑：
  - Rockchip IOMMU groups 仅覆盖多媒体 IP（NPU/RGA/VPU/VOP 等），PCIe 设备不在任何 IOMMU group 中。
  - `rk3588.dtsi` 的 `pcie3x4` / `pcie2x1l0` 节点没有 `iommus` 属性。
  - 因此即使开启 `CONFIG_VFIO`，yt6801 网卡也无法获得 IOMMU 隔离；只能走 `VFIO_NOIOMMU` 非安全模式，不适合生产。
- SR-IOV：
  - `CONFIG_PCI_IOV` 可在软件上开启，但 yt6801 驱动源码中没有 `pci_enable_sriov` / `sriov` 实现。
  - 现有板子无 `sriov_totalvfs`，硬件/驱动均不支持创建 VF。

## 结论

- `CONFIG_VFIO` / `CONFIG_VFIO_PCI`：**软件可开启，但当前硬件/IOMMU 拓扑不支持对 yt6801 的安全 VFIO 直通**；若强开，只能使用 `VFIO_NOIOMMU`（taint 内核，非安全）。
- `CONFIG_PCI_IOV` / SR-IOV VF：**软件可开启，但 yt6801 硬件与驱动不支持 SR-IOV，无法创建 VF**。
- 对 MTL：RK3588 板子不应依赖 DPDK VFIO/PCIe 直通路径；继续使用 native AF_XDP、kernel socket 或其它后端。hugepage 路径不受此限制。
