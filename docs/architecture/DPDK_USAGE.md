# MTL 使用 DPDK 的方式

分析对象：本仓库（版本见 `VERSION`，DPDK 钉版见 `versions.env`）。配套阅读：`ARCHITECTURE.md`（总体架构）、`AF_XDP_EBPF.md`（AF_XDP 后端）、`lib/core.md` / `lib/network.md`。

## 0. 一句话结论

MTL 把 DPDK 作为默认的生产数据面后端：用 DPDK EAL 完成进程/内存/设备初始化，用 `ethdev` 配置网卡队列与描述符，用 mbuf/mempool/ring 完成零拷贝收发，用 Traffic Manager 或 LaunchTime 做 ST 2110-21 硬件 pacing，并用 `rte_flow`/RSS 做接收分流。内核 socket、DPDK AF_XDP/AF_PACKET、native AF_XDP 是同一抽象下的替代后端；DPDK PMD（`MTL_PMD_DPDK_USER`）仍然是性能与功能最完整的路径。

## 1. 依赖与构建

- 库依赖 `libdpdk >= 25.03`（`lib/meson.build`），`versions.env` 钉 DPDK `26.07`。
- 构建时通过 `dependency('libdpdk')` 链接；Windows 走静态 DPDK（`lib/meson.build` 的 `dpdk_root_dir`）。
- 代码大量使用 `RTE_VERSION` 宏做 API 兼容（例如 `RTE_VERSION_NUM(22, 3, 0, 0)`、`RTE_VERSION_NUM(23, 3, 0, 0)`、`RTE_VERSION_NUM(25, 11, 0, 0)`）。
- 需要 hugepage（推荐 2MB）与 VFIO/SR-IOV VF；`mtl_init_params.memzone_max` 可在初始化前调 `rte_memzone_max_set()`。
- DPDK 安装树必须与 MTL 构建树匹配；`dev_eal_loaded_pmd_dir()` 通过 `dladdr(rte_eal_init)` 反查 EAL 所在目录并加载同目录 PMD 插件。

## 2. EAL 初始化（`lib/src/dev/mt_dev.c:dev_eal_init`）

MTL 不把 EAL 参数暴露给应用，而是在 `mt_dev_init()` → `dev_eal_init()` 内部组装 argv：

- 固定参数：进程名 `MT_DPDK_LIB_NAME`、`--file-prefix`、`--match-allocations`（非 Windows）、`--in-memory`。
- 端口：
  - `MTL_PMD_DPDK_USER`：`-a <pci devarg>`（可追加 `rl_burst_size=...`），并计数 `pci_ports`。
  - `MTL_PMD_DPDK_AF_XDP`：`--vdev net_af_xdpN,iface=...,start_queue=...,queue_count=...`。
  - `MTL_PMD_DPDK_AF_PACKET`：`--vdev eth_af_packetN,iface=...,framesz=...,blocksz=...,qpairs=...`。
  - `MTL_PMD_KERNEL_SOCKET` / `MTL_PMD_NATIVE_AF_XDP`：不作为 DPDK 设备处理，保存 `kernel_if`，EAL 侧跳过。
- DMA 设备：每个 `dma_dev_port[i]` 追加 `-a`。
- 主 lcore：`--main-lcore <n>`（若设置）。
- lcore 列表：`-l <main>,<lcores>`（若设置）；DPDK >= 25.11 追加 `--remap-lcore-ids`。
- 无 PCI 端口时追加 `--no-pci`。
- IOVA 模式：`--iova-mode va|pa`（仅用户指定时）。
- 日志级别：`--log-level ...`，并同步 `mt_set_log_global_level()`。
- SIMD：`MTL_FLAG_RXTX_SIMD_512` 时追加 `--force-max-simd-bitwidth=512`。
- PMD 目录：若 `dev_eal_loaded_pmd_dir()` 成功，追加 `-d <dir>`。
- 最后追加 `--` 分隔 EAL 与应用参数。

执行方式与约束：

- `rte_eal_init()` 在独立 pthread 中调用（避免 EAL 默认绑核影响当前线程）。
- 进程内 EAL 只能初始化一次：`static bool eal_initted`，重复 `mtl_init` 返回 `-EIO`。
- `mt_dev_uinit()` 调 `rte_eal_cleanup()`。
- `mtl_init()` 用 `rte_version()` 打印 DPDK 版本。

## 3. 端口与设备模型

### 3.1 驱动信息表（`dev_drvs[]`）

`mt_dev.c` 内置驱动表，把 `rte_eth_dev_info.driver_name` 映射为 MTL 内部类型：

| 字段 | 含义 |
|------|------|
| `port_type` | PF/VF/DPDK AF_XDP/AF_PACKET/内核 socket/native AF_XDP |
| `drv_type` | `MT_DRV_ICE`、`MT_DRV_IAVF`、`MT_DRV_MLX5` 等 |
| `flow_type` | `MT_FLOW_ALL` / `MT_FLOW_NONE` / `MT_FLOW_NO_IP`，决定是否用 Flow Director |
| `rl_type` | 硬件 pacing 类型：`MT_RL_TYPE_TM`（ICE/IAVF 用 Traffic Manager）或 `MT_RL_TYPE_XDP_QUEUE_SYSFS` |
| `flags` | 无 CNI、内核控制、共享 RX 池、无 flow 等能力标记 |

### 3.2 端口初始化（`mt_dev_if_init`）

- 通过 `rte_eth_dev_get_port_by_name()` / `rte_eth_dev_info_get()` 获取 `port_id` 与设备能力。
- 按 PMD 类型计算 TX/RX 队列数（含 system RX 队列、PTP、TAP、HDR split 等）。
- IAVF 强制 TX/RX 队列数对齐。
- 能力探测：
  - `RTE_ETH_DEV_CAPA_RUNTIME_RX_QUEUE_SETUP`
  - `RTE_ETH_TX_OFFLOAD_MULTI_SEGS`
  - `RTE_ETH_TX_OFFLOAD_IPV4_CKSUM`
  - `RTE_ETH_RX_OFFLOAD_TIMESTAMP`
  - `RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP`（LaunchTime/TSN pacing）
- RSS：无 Flow Director 时可启用 `MTL_RSS_MODE_L3` / `MTL_RSS_MODE_L3_L4`，通过 `rte_eth_dev_rss_reta_update()` 配置 RETA。
- NUMA：`rte_eth_dev_socket_id()` 获取端口 socket；用户可 `MTL_PORT_FLAG_FORCE_NUMA` 强制。

### 3.3 端口配置与启停

- `dev_config_port()` 调 `rte_eth_dev_configure(port_id, nb_rx_q, nb_tx_q, &port_conf)`，设置 offload、RSS、ptypes。
- 描述符数量：默认 `MT_DEV_RX_DESC` / `MT_DEV_TX_DESC`（deep TX 用 `MT_DEV_TX_DESC_DEEP`），用户可通过 `nb_tx_desc` / `nb_rx_desc` 覆盖，`rte_eth_dev_adjust_nb_rx_tx_desc()` 对齐硬件限制。
- IAVF + 硬件 RX 时间戳时有描述符数量 workaround。
- `rte_eth_dev_set_ptypes()` 保证 UDP/IPv4/ARP/VLAN 等协议类型解析。
- `dev_start_eth_port()`：IGC 需先 `rte_eth_timesync_enable()`；然后 `rte_eth_dev_start()`；随后启动 timesync。
- 停止/关闭：`rte_eth_dev_stop()` / `rte_eth_dev_close()`；TX 队列可单独 start/stop。

## 4. 队列与内存

### 4.1 队列抽象

`lib/src/datapath/` 提供统一 TX/RX burst：

- `mt_queue.c`：每会话独立队列（Flow Director 可用时）。
- `mt_shared_queue.c`：共享 TX/RX 队列（`MTL_FLAG_SHARED_TX_QUEUE` / `MTL_FLAG_SHARED_RX_QUEUE`）。
- `mt_shared_rss.c`：无 Flow Director 时的 RSS 软件分发。
- `mt_dp_socket.c`：内核 socket 后端的统一适配。

RX 分流优先级：Flow Director > 共享队列 + 软件分发 > RSS。

### 4.2 描述符与队列 setup

- `rte_eth_rx_queue_setup()` / `rte_eth_tx_queue_setup()`，传入 socket_id、rx/tx conf、mempool。
- 队列深度与 `nb_rx_desc` / `nb_tx_desc` 一致。
- TX deep 模式下使用更深 ring 与 pad mbuf。

### 4.3 Mempool / mbuf

- `mt_mempool_create_by_ops()` 封装 `rte_pktmbuf_pool_create_by_ops()`：
  - 元素数优化为 `2^q - 1`。
  - cache_size 对齐。
  - 名称追加 `_<mempool_idx>`（保证恢复周期唯一）。
  - data_room_size = element_size + `MT_MBUF_HEADROOM_SIZE`。
  - NUMA socket 感知。
- hugepage 内存注册为 mempool backing；AF_XDP 后端直接把 mempool 内存注册为 UMEM。
- `mt_rte_malloc_socket()` / `mt_rte_zmalloc_socket()` / `mt_rte_free()` 封装 `rte_malloc_socket` / `rte_free`，用于控制面/内部结构；数据面避免 malloc。

### 4.4 rte_ring

- 视频 TX builder 与 transmitter 之间用 `rte_ring` 连接。
- TX ring 使用 `rte_ring_sp_enqueue_bulk()`（全有或全无），避免 RTP 序列与 pacing 被部分入队破坏。
- `mt_ring_dequeue_clean()` 用 `rte_ring_sc_dequeue()` 清理。

## 5. 收发数据路径与 DPDK API

### 5.1 TX

- 应用/会话通过 `mt_txq_burst()` 发包，底层调 `rte_eth_tx_burst()`。
- 短发（返回值小于请求）时，MTL 把剩余包暂存 `inflight[]`，下轮重发；不丢弃、不循环重试。
- TX 回收：`rte_eth_tx_done_cleanup()` / descriptor reclaim（`dev_tx_deep` 相关）。
- 零拷贝：RTP payload 的 extbuf 直接指向帧缓冲，避免 memcpy。
- TX 校验回调：`rte_eth_add_tx_callback()` 注册 `dev_tx_pkt_check()`，校验 mbuf 长度/分段，异常包替换为 pad。

### 5.2 RX

- `rte_eth_rx_burst()` 批量收包。
- mbuf 转换为 ST2110 帧：按 RTP timestamp 找 slot，bitmap 去重，memcpy 或 DMA 写入预分配帧缓冲。
- 帧缓冲必须零初始化，丢包缺口读为零。
- `recv_size >= frame_size && dma_nb == 0` 时交付应用。

### 5.3 多段与 offload

- `MT_IF_FEATURE_TX_MULTI_SEGS` 控制是否启用 multi-seg offload。
- IPv4 checksum offload 可选。
- LaunchTime offload 通过 mbuf dynflag/dynfield 设置发送时间戳。

## 6. Pacing 与硬件限速

ST 2110-21 pacing 是 MTL 的核心，DPDK 提供两种硬件手段：

### 6.1 Traffic Manager（`ST21_TX_PACING_WAY_RL`）

- 仅 ICE/IAVF 等 `rl_type == MT_RL_TYPE_TM` 的驱动支持。
- `rte_tm_shaper_profile_add()` 创建 shaper profile。
- `rte_tm_node_add()` 为每个 TX 队列挂节点（PF/VF 层级）。
- `rte_tm_hierarchy_commit()` 提交层级。
- 运行时可 `rte_tm_node_delete()` / 重建调整。
- 硬件 TM 节流避免 CPU 忙等；不支持的驱动回落 TSC 软件 pacing。

### 6.2 LaunchTime / TSN（`ST21_TX_PACING_WAY_TSN`）

- 需要 `RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP`。
- 注册 mbuf dynflag（`RTE_MBUF_DYNFLAG_TX_TIMESTAMP_NAME`）与 dynfield（`RTE_MBUF_DYNFIELD_TIMESTAMP_NAME`）。
- 在 mbuf 中写入目标发送时间，NIC 按时间戳发送。

### 6.3 TSC 软件 pacing（默认/回退）

- `mt_get_tsc()` / `mt_wait_tsc_stable()` 使用 TSC；不依赖 DPDK 硬件 pacing。
- RK3588 等无 TM 能力的平台使用此方式。

## 7. Flow Director、RSS 与组播

- 若 `flow_type == MT_FLOW_ALL`，MTL 用 `rte_flow_create()` / `rte_flow_validate()` / `rte_flow_destroy()` 为每个会话建立 UDP 目的端口规则，把包导向独立 RX 队列。
- 若 `MT_FLOW_NONE`，启用 RSS 软件分发或 RSS 硬件 RETA。
- 组播：`rte_eth_dev_set_mc_addr_list()` 管理组播 MAC；IGMPv3 由库内 `mt_mcast.c` 生成。
- ARP 解析由库内 `mt_arp.c` 完成（DPDK 绕过内核）。

## 8. Timesync 与 PTP

- `rte_eth_timesync_enable()` / `read_time()` / `read_tx_timestamp()` / `read_rx_timestamp()` / `adjust_time()` / `adjust_freq()`。
- IGC 需要在端口 start 前/后分别 enable 并校验。
- 硬件 PTP 约 30 ns；无硬件 timesync 的平台（如 RK3588 板载网卡）可用外部 `ptp_get_time_fn` 或 TSC 源。
- LaunchTime 依赖 timesync offload。

## 9. DMA offload（`rte_dmadev`）

- `mt_dma.c` 使用 DPDK DMA 设备做 RX 路径 mem-to-mem 拷贝：
  - `rte_dma_count_avail()` 发现设备。
  - `rte_dma_configure()` / `rte_dma_vchan_setup()` / `rte_dma_start()`。
  - `rte_dma_copy()` / `submit()` / `completed()`。
- `dma_copy_test()` 启动时验证拷贝正确性。
- `rte_dev_dma_map()` / `unmap()` 用于外部内存（如 GPU）DMA 映射。

## 10. 统计、诊断与扩展

- `rte_eth_stats_get()` / `rte_eth_stats_reset()`：基础收发计数。
- `rte_eth_xstats_get()` / `rte_eth_xstats_get_names()` / `reset()`：扩展统计。
- `rte_eth_link_get_nowait()`：链路状态。
- `rte_eth_dev_get_supported_ptypes()`：协议类型。
- `mt_pcap.c` 可基于 DPDK mbuf 抓包。
- USDT 探针（`MTL_HAS_USDT`）用于运行时观测。

## 11. 与其它后端的关系

| 后端 | 是否初始化 EAL | 是否使用 ethdev | 说明 |
|------|----------------|-----------------|------|
| `MTL_PMD_DPDK_USER` | 是 | 是 | 主路径，kernel bypass |
| `MTL_PMD_DPDK_AF_XDP` | 是 | 是（vdev） | DPDK 封装的 AF_XDP，实验性 |
| `MTL_PMD_DPDK_AF_PACKET` | 是 | 是（vdev） | DPDK 封装的 AF_PACKET，实验性 |
| `MTL_PMD_KERNEL_SOCKET` | 是（无 PCI） | 否 | 走内核协议栈，实验性 |
| `MTL_PMD_NATIVE_AF_XDP` | 是（无 PCI/或有） | 否 | native xsk，需要 MtlManager |

即使使用非 DPDK 数据面，MTL 仍可能使用 `rte_malloc`/`rte_ring`/`rte_mempool` 等 DPDK 基础设施；native AF_XDP 的 mempool 内存直接注册为 UMEM。

## 12. RK3588 / ARM 注意事项

- DPDK 需交叉编译并安装到 sysroot；MTL 通过 `dpdk_root_dir` / pkg-config 找到。
- ARM 上通常没有 ICE/E810 的 Traffic Manager，pacing 回落 TSC 软件方式。
- hugepage 与 IOMMU/VFIO 在嵌入式内核上需确认（RK3588 板载网卡可能不支持 native XDP，需退 SKB/copy）。
- `MTL_FLAG_TASKLET_THREAD` 可在容器/无绑核环境使用 pthread 模式。

## 13. 关键源文件映射

| 文件 | DPDK 相关职责 |
|------|---------------|
| `lib/src/dev/mt_dev.c` | EAL init、端口配置、队列、mempool、TM、timesync、flow |
| `lib/src/dev/mt_dev.h` | EAL argv 预算、端口常量 |
| `lib/src/dev/mt_af_xdp.c` | native AF_XDP（UMEM = mempool 内存） |
| `lib/src/mt_main.c` | 实例生命周期、NUMA 绑定、socket_id |
| `lib/src/mt_util.c` | `mt_rte_*`、mempool create/free、ring 清理 |
| `lib/src/mt_dma.c` / `mt_dma_auto.c` | `rte_dmadev` |
| `lib/src/datapath/mt_queue.c` / `mt_shared_queue.c` / `mt_shared_rss.c` | 队列与 RSS |
| `lib/src/mt_flow.c` | `rte_flow` |
| `lib/src/mt_stat.c` | stats/xstats |
| `lib/src/mt_ptp.c` | timesync/PHC |
| `lib/meson.build` | DPDK 依赖与版本 |
