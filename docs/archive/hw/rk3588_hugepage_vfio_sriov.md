# RK3588 开发板 hugepage / VFIO / SR-IOV 支持情况

## 平台

- 板子：RK3588，内核 `5.10.226-g40733b7b2aa6-dirty`，aarch64
- 网卡：2 × Motorcomm yt6801（PCIe，`0002:21:00.0`、`0003:31:00.0`）
- 访问：`ssh board`

## 结论摘要（先看这里）

1. **hugepage：可用。** 2MB hugepage 已就绪，MTL 的 hugepage 需求不受本板限制。
2. **VFIO 安全直通：在本板硬件拓扑下不成立。** 不是"内核没编译"这么简单——即使打开
   `CONFIG_VFIO`/`CONFIG_VFIO_PCI`，RK3588 的 IOMMU 也不覆盖 PCIe 控制器，
   yt6801 不在任何 IOMMU group 中，**无法获得 DMA 隔离**。
3. **VFIO_NOIOMMU：不是可行的生产退路。** 它放弃全局 DMA 隔离、taint 内核，
   对"可靠嵌入式设备"同样不适用（详见"为什么 no-iommu 不能作为可靠嵌入式方案"）。
4. **SR-IOV：不可用。** 内核未开 `CONFIG_PCI_IOV`，且 yt6801 驱动/硬件均无 SR-IOV 实现。
5. **对 MTL：不要依赖 DPDK VFIO/PCIe 直通路径。** 继续使用 native AF_XDP、
   kernel socket 等后端；hugepage 路径不受影响。

**一句话：本板的主要障碍是"PCIe 无 IOMMU 覆盖 + 网卡无 SR-IOV"，而不是"内核缺配置"。
重编内核只能解决后者，解决不了前者。**

---

## 2MB hugepage

- 状态：**支持**
- 证据：
  - `CONFIG_HUGETLBFS=y`，`CONFIG_SYS_SUPPORTS_HUGETLBFS=y`
  - `/sys/kernel/mm/hugepages/hugepages-2048kB` 存在
  - `hugetlbfs` 已挂载到 `/dev/hugepages`
  - 实测写入 `vm.nr_hugepages=4` 后 `HugePages_Total: 4`（8MB），`HugePages_Free: 4`
- 结论：MTL 的 DPDK hugepage 需求可满足，2MB hugepage 可用。

---

## VFIO 与 IOMMU 拓扑（本板的核心限制）

### 现象

- 当前内核 `# CONFIG_VFIO is not set`，无 `/dev/vfio/`，模块树无 vfio 模块。
- `CONFIG_IOMMU_SUPPORT=y`，系统中存在 IOMMU groups，但**这些 group 只覆盖多媒体 IP**
  （NPU/RGA/VPU/VOP 等）。
- `rk3588.dtsi` 中 `pcie3x4` / `pcie2x1l0` 节点**没有 `iommus` 属性**。
- 两个 yt6801 设备（`0002:21:00.0`、`0003:31:00.0`）**不属于任何 IOMMU group**。

### 关键判断

VFIO 的安全前提是设备能被 IOMMU 隔离。本板的情况是：

- **软件层面**：`CONFIG_VFIO`、`CONFIG_VFIO_PCI` 在 Kconfig 上确实可以打开
  （`VFIO` select `IOMMU_API`，`VFIO_PCI` 依赖 `VFIO && PCI && EVENTFD && MMU`，
  ARM64 均满足）。
- **硬件/拓扑层面**：PCIe 控制器没有接入 IOMMU，yt6801 的 DMA 不受 IOMMU 约束。
  即使编译并加载 `vfio-pci`，该设备也只能以 **no-iommu 模式**绑定。

**因此，"重编内核开 VFIO"并不能得到可用的安全直通；它只会把问题从
"没有 VFIO 接口"变成"有 VFIO 接口但只能不安全地使用"。**

### 若要真正解决

需要在 **DTS 中为 PCIe 控制器补充 `iommus` 绑定**，确认 RK3588 的 IOMMU
（及 Rockchip SDK）支持 PCIe 侧设备，并验证 DMA 翻译/隔离实际生效。
在这之前，VFIO 直通在本板上不具备生产可用性。

---

## VFIO_NOIOMMU 的定位（明确不推荐）

`CONFIG_VFIO_NOIOMMU` 可在无 IOMMU 时提供 VFIO 接口，但：

- 设备 DMA 可访问**整个物理内存**，无任何硬件边界；
- 内核会被 taint（`TAINT_UNSAFE_VFIO`）；
- 它解决的是"接口有无"，不是"隔离有无"。

**为什么 no-iommu 不能作为"可靠嵌入式"方案：**

1. **"设备可信"不等于"DMA 安全"。** no-iommu 的风险来自驱动/firmware 的 DMA 编程错误，
   而不是来自不可信用户。嵌入式栈再封闭，也无法消除网卡驱动描述符错误、
   错误路径 DMA、固件异常等导致的越界写。
2. **本板风险更高。** yt6801 是相对小众的网卡，驱动成熟度与错误路径验证有限；
   PCIe 侧又完全没有 IOMMU 兜底，一次 DMA 越界即可破坏内核内存或 panic。
3. **收益很低。** yt6801 不支持 SR-IOV，VFIO 只能整卡绑定：宿主机将失去该网卡，
   两块都直通则可能丢失管理/SSH 通道，而换来的只是一条 DPDK PMD 物理网卡路径。
4. **不可承诺隔离。** 对"可靠"定位的产品，无法向使用方承诺内存隔离，是根本性缺陷。

**结论：no-iommu 仅可用于受控实验/调试，不应作为该板的生产或"可靠嵌入式"方案。**

---

## SR-IOV VF

- 状态：**不可用**
- 证据：
  - `# CONFIG_PCI_IOV is not set`
  - 两个 yt6801 设备均无 `sriov_totalvfs` 文件
  - 驱动源码中无 `pci_enable_sriov` / `sriov` 相关实现
  - 未启用 `CONFIG_UIO_PCI_GENERIC`（仅 `CONFIG_UIO`、`CONFIG_STMMAC_UIO`）
- 结论：
  - `CONFIG_PCI_IOV` 可在软件上开启，但 **yt6801 驱动与硬件均不支持 SR-IOV**，
    无法创建 VF；
  - 因此"用 VF 做多实例直通/隔离"的路线在本板上不成立。

---

## 对 MTL 的影响与建议

- `MTL_PMD_DPDK_USER` 的 PCI 直通路径在本板**不可用**：
  - 当前内核无 VFIO；
  - 即使开启 VFIO，PCIe 无 IOMMU 覆盖，只能 no-iommu（不安全，不推荐）；
  - 无 UIO PCI generic 作为替代。
- **推荐后端**：`MTL_PMD_NATIVE_AF_XDP`、`MTL_PMD_KERNEL_SOCKET`、
  `MTL_PMD_DPDK_AF_PACKET` 等，不依赖 VFIO/IOMMU 隔离，符合本板拓扑。
- **hugepage 路径不受影响**，可正常使用 2MB hugepage。
- 若未来确需 DPDK 物理网卡直通，前置条件是：
  1. DTS 为 PCIe 控制器接入 IOMMU 并验证隔离生效；
  2. 内核开启 `CONFIG_VFIO`、`CONFIG_VFIO_PCI`；
  3. 评估整卡绑定对管理网络的影响（必要时保留独立管理链路）。
  仅完成第 2 步不足以构成可用方案。

---

## 编译服务器内核源码分析（`ssh devs`）

- 源码目录：`/data/kkkqkx/rk3588_5.1_XDP`（Rockchip SDK，内核 5.10，GKI/rockchip 配置）
- `.config` / `rockchip_linux_defconfig` / `ztl_defconfig` 中均未开启
  `CONFIG_VFIO`、`CONFIG_VFIO_PCI`、`CONFIG_PCI_IOV`
- Kconfig 依赖检查：
  - `VFIO`：`menuconfig VFIO`，`select IOMMU_API`，
    `select VFIO_IOMMU_TYPE1 if MMU && (X86 || S390 || ARM || ARM64)`。ARM64 满足，软件可开启。
  - `VFIO_PCI`：`depends on VFIO && PCI && EVENTFD` 且 `depends on MMU`。软件可开启。
  - `VFIO_NOIOMMU`：无 IOMMU 时的非安全回退，会 taint 内核，不作为生产方案。
- 硬件/IOMMU 拓扑：
  - Rockchip IOMMU groups 仅覆盖多媒体 IP，PCIe 设备不在任何 IOMMU group 中。
  - `rk3588.dtsi` 的 `pcie3x4` / `pcie2x1l0` 节点没有 `iommus` 属性。
  - 这是本板 VFIO 直通不可用的**根本原因**，非内核配置问题。
- SR-IOV：
  - `CONFIG_PCI_IOV` 可软件开启，但 yt6801 驱动无 SR-IOV 实现，硬件无 `sriov_totalvfs`。

---

## 最终结论

| 项目 | 状态 | 说明 |
|---|---|---|
| 2MB hugepage | ✅ 可用 | 满足 MTL hugepage 需求 |
| VFIO 安全直通 | ❌ 不可用 | PCIe 无 IOMMU 覆盖，yt6801 不在 IOMMU group；非内核配置问题 |
| VFIO_NOIOMMU | ⚠️ 仅实验 | 无 DMA 隔离、taint 内核，不适用于可靠嵌入式/生产 |
| SR-IOV VF | ❌ 不可用 | 内核未开 `CONFIG_PCI_IOV`，且 yt6801 驱动/硬件不支持 |
| DPDK 物理网卡 PMD | ❌ 不建议 | 依赖上述不可用路径；改用 AF_XDP / kernel socket 等后端 |

**主要问题定位：本板的瓶颈是硬件/IOMMU 拓扑（PCIe 未接 IOMMU）与网卡能力（无 SR-IOV），
而非内核缺少 VFIO 配置。重编内核不能解决主要问题；no-iommu 也不是可靠嵌入式的合理退路。**
