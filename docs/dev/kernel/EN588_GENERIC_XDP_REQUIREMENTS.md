# EN588：generic XDP 的内核需求与镜像修改结论

## 1. 背景

当前准备使用 generic XDP，而不是 native/driver XDP。本文件用于判断是否仍需补充
`BPF_JIT`、`XDP_SOCKETS_DIAG`、`HUGETLBFS`、`BTF`，以及是否需要修改 Ubuntu 镜像。

## 2. ssh en588-2 原始内核观察

原始 Ubuntu 内核（`6.1.118`）已具备：

- `CONFIG_XDP_SOCKETS=y`
- `CONFIG_BPF_SYSCALL=y`
- `CONFIG_PTP_1588_CLOCK=y`
- `CONFIG_NET_PTP_CLASSIFY=y`

原始内核未开启：

- `CONFIG_BPF_JIT`
- `CONFIG_XDP_SOCKETS_DIAG`
- `CONFIG_HUGETLBFS`（需单独确认，MTL 通常依赖大页）
- `CONFIG_DEBUG_INFO_BTF`

网卡 `yt6801` 仅支持 software receive timestamp，没有 PHC，因此 PTP 仍应按单向软件方案处理。

## 3. generic XDP 的最小内核需求

generic XDP 本身主要需要：

- `CONFIG_XDP_SOCKETS=y`
- `CONFIG_BPF_SYSCALL=y`
- 内核 XDP 核心支持

它不强依赖：

- `CONFIG_BPF_JIT`：JIT 是性能优化，不是 generic XDP 的功能前提；无 JIT 时可使用 BPF 解释执行（取决于内核配置）。
- `CONFIG_XDP_SOCKETS_DIAG`：仅用于诊断/查询，不影响数据面。
- `CONFIG_DEBUG_INFO_BTF`：仅对 CO-RE/BTF 依赖的程序需要；普通 XDP 程序不必须。
- `CONFIG_HUGETLBFS`：不是 XDP 需求，但 MTL 自身可能需要大页内存。

## 4. 是否需要补这些配置

| 配置 | generic XDP 是否必需 | 结论 |
|---|---:|---|
| `BPF_JIT` | 否 | 可跳过；仅性能下降 |
| `XDP_SOCKETS_DIAG` | 否 | 可跳过；仅诊断能力缺失 |
| `BTF` | 否 | 可跳过；除非使用 CO-RE/BTF 程序 |
| `HUGETLBFS` | 否（对 XDP） | XDP 不需要；但 MTL 若使用大页，则仍需要 |

因此，如果后续**完全使用 generic XDP**，可以跳过 `BPF_JIT`、`XDP_SOCKETS_DIAG`、`BTF` 的内核修改。

`HUGETLBFS` 是否保留应由 MTL 的大页需求决定，而不是由 XDP 决定。

## 5. 大页是否需要

需要。

虽然 generic XDP 本身不依赖大页，但 MTL 内部使用 DPDK（`rte_eal_init`、
`rte_malloc_socket`/`rte_zmalloc_socket`），运行时依赖 hugepages。RK3588 单板
README 也明确要求配置 hugepages（建议 256–512 页）。

因此：

- `HUGETLBFS` 不能因为 “使用 generic XDP” 而跳过。
- Ubuntu 原始内核当前 `CONFIG_HUGETLBFS` 未开启，若要运行 MTL，必须启用它。
- `BPF_JIT`、`XDP_SOCKETS_DIAG`、`BTF` 仍可跳过。

## 6. 对镜像修改的结论

- Ubuntu 原始内核已具备 `CONFIG_XDP_SOCKETS=y` 和 `CONFIG_BPF_SYSCALL=y`。
- generic XDP 本身不需要额外内核配置。
- 但 MTL 需要 `CONFIG_HUGETLBFS=y`，因此若运行 MTL，不能完全不动内核。
- PTP 单向设计不需要内核/镜像修改，继续按现有文档实施。
- 只有 `HUGETLBFS` 是当前运行 MTL 的必要内核能力；`BPF_JIT`、`XDP_SOCKETS_DIAG`、`BTF` 可跳过。

## 6. 推荐策略

1. 直接在 Ubuntu 原始内核上使用 generic XDP。
2. 不修改 img，不迁移 0002/0003/0004。
3. 仅在 MTL 实测失败且明确指向 BPF JIT/BTF/HUGETLBFS 时，再做最小内核调整。
4. PTP 继续按 `RK3588_ONE_WAY_PTP_SYNC_DESIGN.md` 的单向软件方案实施。
