# RK3588 YT6801 驱动 AF_XDP 缺口分析

## 背景

`docs/issue/RK3588_DRIVER_ISSUES.md` §1 指出 YT6801 网卡驱动缺少 XDP 能力，导致 MTL
`native_af_xdp` 后端无法以 native/zero-copy 模式运行。本文基于
`/data/kkkqkx/rk3588_5.1_XDP` SDK 的内核源码，对 AF_XDP 所需的内核态与驱动态条件做
缺口分析，结论写入 `docs/archive/hw/`。

分析对象：Rockchip SDK `rk3588_5.1_XDP`，内核 `5.10.226`，网卡驱动
`drivers/net/ethernet/yt6801`（Motorcomm Fuxi GMAC，PCI `1F0A:6801`）。

## 1. 内核态能力：已具备

SDK 内核配置 `.config` 中 AF_XDP 相关选项已开启：

- `CONFIG_XDP_SOCKETS=y`
- `CONFIG_XDP_SOCKETS_DIAG=y`
- `CONFIG_BPF_SYSCALL=y`
- `CONFIG_BPF_JIT=y`
- `CONFIG_BPF_JIT_ALWAYS_ON=y`
- `CONFIG_PAGE_POOL=y`
- `CONFIG_HUGETLBFS=y`

内核源码树包含完整 AF_XDP 子系统：`net/xdp/xsk.c`、`xdp_umem.c`、`xsk_buff_pool.c`、
`xskmap.c`。因此 AF_XDP socket 创建、UMEM 注册、XSK map 重定向等内核能力不是缺口；
本 SDK 的内核态条件优于当前运行板的 6.1/Ubuntu 基线（hugepage/BPF_JIT/BTF 问题见
`docs/env/RK3588_5.10_AF_XDP_EBPF.md`）。

唯一仍缺失的内核级能力是 `CONFIG_KPROBES`（未开启），它只影响 `tools/ebpf/` 诊断工具，
不影响 AF_XDP 数据面。

## 2. 驱动态缺口：YT6801/Fuxi GMAC 不支持 XDP

驱动源码目录：`kernel/drivers/net/ethernet/yt6801/`。对驱动源码做关键字检索，结果为
零命中：

- `ndo_bpf`：0 命中
- `ndo_xdp_xmit`：0 命中
- `xdp_rxq_info` / `xdp_do_flush` / `xdp_return_frame`：0 命中
- `xsk_pool` / `xsk_buff_pool`：0 命中
- `page_pool`：0 命中

### 2.1 `net_device_ops` 无 XDP 入口

`fuxi-gmac-net.c:2304` 定义的 `fxgmac_netdev_ops` 包含 open/stop/start_xmit/tx_timeout/
get_stats64/change_mtu/set_mac_address/ioctl/vlan/poll_controller/set_features/set_rx_mode，
但没有 `.ndo_bpf` 与 `.ndo_xdp_xmit`。内核 XDP 子系统通过 `ndo_bpf` 安装/卸载 BPF 程序，
通过 `ndo_xdp_xmit` 完成 XDP TX 重定向；两者缺失意味着 native XDP 无法挂载，这也是
`ip link set ... xdpdrv` 返回 `Underlying driver does not support XDP in native mode` 的直接
原因。

### 2.2 RX 路径是 skb 路径，无 XDP hook

`fuxi-gmac-net.c:2614` 的 `fxgmac_rx_poll()` 在收到完整帧后调用
`fxgmac_create_skb()`（`fuxi-gmac-net.c:2373`）分配 `sk_buff`，最终走 `napi_gro_receive()`
（`fuxi-gmac-net.c:2815`）。整个 RX 路径没有：

- `xdp_rxq_info` 注册与 `xdp_buff` 构造；
- `bpf_prog_run_xdp()` 调用；
- `XDP_PASS`/`XDP_DROP`/`XDP_REDIRECT`/`XDP_TX` 分支处理；
- `xdp_do_flush()` 批量刷新。

因此即使内核 XDP socket 可用，驱动也不会把收到的包送入 XSK map，native AF_XDP 无法收包。

### 2.3 无 page pool / XSK buffer pool

驱动 RX 描述符刷新与 skb 分配使用常规内存分配，没有 `page_pool` 或 `xsk_buff_pool`。
AF_XDP zero-copy 要求驱动把 RX 描述符指向 UMEM 页/XSK buffer，并在完成时把 buffer
交还 XSK 队列；当前驱动结构不支持该内存模型。`page_pool` 虽在内核配置中开启，但驱动
未使用。

### 2.4 TX 路径是标准 netdev xmit，无 XDP TX

`fuxi-gmac-net.c:1936` 的 `fxgmac_xmit()` 是标准 `ndo_start_xmit` 路径，`fxgmac_tx_poll()`
（`fuxi-gmac-net.c:2447`）负责完成中断回收。没有 `ndo_xdp_xmit`，XDP_TX 与 AF_XDP
socket 的 TX 重定向无法由驱动发出。MTL 的 TX 数据面只能继续使用普通网络栈/AF_XDP
copy 模式的 TX ring，无法利用 XDP 快速路径。

### 2.5 仅残留的历史 XDP 痕迹

`fuxi-gmac-net.c:1682-1683` 仅剩注释：

```c
/* synchronize_rcu() needed for pending XDP buffers to drain */
//if (adapter->xdp_ring[0]) 20210709
    synchronize_rcu();
```

说明该驱动早期可能参考过支持 XDP 的实现，但当前版本的 `xdp_ring` 字段与相关逻辑已移除，
`synchronize_rcu()` 只是残留调用。这不是功能支持，不能作为 XDP 能力证据。

### 2.6 队列与限速条件不满足 MTL 的 native 路径假设

MTL native AF_XDP 依赖 manager 为每个 XSK 分配队列号，并通过 sysfs
`/sys/class/net/<if>/queues/tx-<q>/tx_maxrate` 做队列级限速。YT6801 实测/源码条件为：

- TX 侧实际只有 `tx-0`（单 TX 队列），虽然 RX 侧有多队列；
- `tx_maxrate` 写入返回 `EOPNOTSUPP`；
- MTL 的 RL 探测代码只对驱动名 `ice` 执行，YT6801 不会被探测。

因此即使补上 XDP hook，队列级硬件限速仍不可用，MTL 必须继续使用 TSC 软件 pacing。

## 3. 对 MTL `native_af_xdp` 的影响

1. `mt_dev_xdp_init()`（`lib/src/dev/mt_af_xdp.c`）可以在内核/UMEM 层完成初始化，但
   manager 的 `load_xdp()` 会先尝试 `XDP_MODE_NATIVE`，失败后退化到 `XDP_MODE_SKB`。
2. MTL 应用仍可运行，但 AF_XDP 只能是 generic XDP + copy UMEM 形态，无法 zero-copy；
   性能基准必须按 copy 模式设定。
3. 若 generic XDP 也不可用，`file_playout`/live ingest 已支持按端口名切换到 `kernel:`
   后端，这是当前最稳妥的回退路径。
4. 要获得 native/zero-copy 性能，必须修改 YT6801 驱动：实现 `ndo_bpf`、RX 路径 XDP
   处理、`ndo_xdp_xmit`、page_pool/XSK buffer 管理，并验证 `XDP_ZEROCOPY` 绑定。

## 4. 缺口分级

| 缺口 | 级别 | 说明 |
|---|---|---|
| `ndo_bpf` 缺失 | 阻塞 native XDP | `xdpdrv` 无法挂载 |
| RX 路径无 XDP hook | 阻塞 native AF_XDP 收包 | 包不会进入 XSK map |
| `ndo_xdp_xmit` 缺失 | 阻塞 XDP_TX / AF_XDP TX redirect | TX 快速路径不可用 |
| page_pool/XSK buffer pool 缺失 | 阻塞 zero-copy | 只能 copy UMEM |
| `tx_maxrate` 不支持 | 阻塞队列级 pacing | 必须 TSC 软件 pacing |
| `CONFIG_KPROBES` 未开启 | 非数据面阻塞 | 仅影响诊断工具 |

## 5. 结论

本 SDK 的内核已具备 AF_XDP 所需的基础能力，缺口集中在 YT6801 驱动。驱动当前完全按 skb
网络栈实现，缺少 XDP native 入口、RX/TX XDP 处理、page pool/XSK buffer 管理与队列限速。
因此 RK3588 上的 MTL AF_XDP 只能以 generic XDP + copy UMEM 或 `kernel:` 后端运行；若要
native/zero-copy，需要驱动侧补全 XDP 支持，而不是仅调整内核配置。

## 参考

- `docs/issue/RK3588_DRIVER_ISSUES.md` §1
- `docs/env/RK3588_5.10_AF_XDP_EBPF.md`
- `docs/architecture/AF_XDP_EBPF.md`
- SDK 路径：`/data/kkkqkx/rk3588_5.1_XDP/kernel/drivers/net/ethernet/yt6801`
- 证据：`fuxi-gmac-net.c:2304`（netdev_ops）、`:2614`（rx_poll）、`:2373`（create_skb）、
  `:2815`（napi_gro_receive）、`:1936`（xmit）、`:2447`（tx_poll）、`:1682-1683`（XDP 残留注释）
