# lib/src 网络服务：CNI 与协议栈

> 源文件：`mt_cni.c/h`、`mt_arp.c/h`、`mt_dhcp.c/h`、`mt_mcast.c/h`、`mt_flow.c/h`、`mt_socket.c/h`、`mt_tap.c/h`、`mt_pcap.c/h`

媒体端口被 DPDK 接管后，ARP/DHCP/IGMP/PTP 等非媒体报文不再经过内核协议栈。本组文件在用户态重建了这些控制面协议，并把 DPDK 口和内核侧（TAP / kernel socket）桥接起来。

---

## `mt_cni.c` / `mt_cni.h`

**一句话**：CNI（Control Network Interface）——媒体端口上"非媒体报文"的收发枢纽：按协议分发到 arp/dhcp/mcast/ptp 模块，其余转发给内核；同时提供共享接收队列 CSQ 与 RX pcap dump。

### 收包分类（`cni_rx_handle`，198）

对 CNI 专用 RX 队列上收到的帧按 EtherType/UDP 端口分派：

- ARP → `mt_arp_parse`；DHCP（67/68）→ `mt_dhcp_parse`；IGMP → `mt_mcast_parse`；PTP（319/320）→ `mt_ptp_parse`；
- 其余 IPv4/UDP：`cni_udp_handle`(166) 未命中已知协议时——
  - `cni_udp_detect_analyses`(64)：对未知 UDP 流做统计分析（辅助发现"看起来是 ST2110 媒体流但没建 session"的端口），`cni_udp_detect_dump` 打印；
  - `cni_burst_to_kernel`(128)：把报文转交给内核侧对应接口（Windows 走 `mt_tap`，Linux kernel-port 场景走 dp_socket）。
- 反方向 `cni_burst_from_kernel`(145)：内核侧产生的控制报文（如系统 ping/IGMP）经 CNI 发到线上。

### 运行形态（`cni_init`，559 起）

- `lcore_tasklet = true`：CNI 作为 tasklet 挂到 main scheduler 轮询（`cni_tasklet_start/stop`）；
- `lcore_tasklet = false`：起专用线程 `cni_traffic_thread`(391) 跑 `cni_traffic`(337)；
  两者互斥——tasklet 启动时停线程，tasklet 停止时恢复线程。
- CNI 的 RX 走独立队列（`ST_CNI_RX_BURST_SIZE 32`），并在此队列上做 pcap dump：`cni_start_pcap`(297)（默认最多 10000 包）/ `cni_stop_pcap` / `cni_dump_pcap`，底层用 `mt_pcap.c`。

### CSQ 共享控制队列（`mt_csq_*`）

- `mt_csq_get(impl, port, flow)`：会话按 `mt_rxq_flow` 申请一个"借用 CNI 队列"的接收入口（`mt_csq_entry`，挂在 `cni->csq_queues` 链表，`mt_csq_put` 释放）；
- `mt_csq_burst`：从 CNI 队列收包后按流过滤。
- 唯一消费方是 `datapath/mt_queue.c:86`（共享队列模式下没有专用 RSS 队列时的回退路径），`csq_stat`(102) 进统计。

`mt_cni.h` 只暴露 `mt_cni_init/uinit/start/stop` 与 `mt_csq_*`。

---

## `mt_arp.c` / `mt_arp.h`

**一句话**：用户态 ARP 客户端/应答器——为 TX 会话解析目的（网关）MAC，并回应针对本端 IP 的 ARP 请求。

- 表：每端口 `mt_arp_impl`，容量 `MT_ARP_ENTRY_MAX`（60，`mt_main.h`）。
- `mt_arp_get_mac(impl, dip, &ea, port, timeout_ms)`(358)：查表未命中则发 ARP request（经 CNI TX），以 `timeout_ms` 阻塞轮询等待 `mt_arp_parse` 填入 reply——**只能在控制面调用**（数据面 tasklet 内会阻塞饿死同调度器其它 tasklet）。TX 会话建流时经 `mt_main.c mt_dst_ip_mac` 使用。
- `mt_arp_parse`(250)：处理 CNI 分来的 ARP 报文——reply 入表；request 若目标是本端 IP 则回 reply（`mt_arp_cni_get_mac`(263) 组帧）。
- 支持 VLAN 端口的 ARP（带 vlan id 组帧）。

---

## `mt_dhcp.c` / `mt_dhcp.h`

**一句话**：极简 DHCP 客户端——没配静态 IP 的端口可从 DHCP 服务器拿 ip/netmask/gateway。

- `struct mt_dhcp_hdr`（`mt_dhcp.h`，RFC2131 布局 + options 变长）。
- 状态机走 DISCOVER→（OFFER）→REQUEST→（ACK）→RENEW/REBIND，超时重传由内部 alarm 驱动；`mt_dhcp_parse`(439) 处理 CNI 分来的 OFFER/ACK/NAK，`mt_dhcp_init`(520)/`uinit`(571) 管理端口实例。
- 结果读取：`mt_dhcp_get_ip/get_netmask/get_gateway`，`mt_main.c`/`mt_dev.c` 在 init 时用它填充接口地址（用户未显式配置且开 `MTL_FLAG_DHCP` 时）。

---

## `mt_mcast.c` / `mt_mcast.h`

**一句话**：IGMPv3 客户端——RX 会话加入/离开组播组（含 SSM 源过滤），并周期刷新成员关系。

- `mt_mcast_join(impl, group, source, port)`(561) / `mt_mcast_leave`(667)：维护 `MT_MCAST_GROUP_MAX`（60）条记录；join 后立即发 Membership Report，并按 `IGMP_JOIN_GROUP_PERIOD_S`（10s）在 `mt_mcast_parse`/定时路径重发以维持交换机组播树。
- `mt_mcast_parse`(782)：处理 Querier 的 General/Group Specific Query（含 S），在 224.0.0.22 回 Report（DSCP `0xc0`）；组记录类型 `MCAST_MODE_IS_INCLUDE` 等按 IGMPv3 语义生成 change/allow/block 记录。
- `mt_mcast_l2_join/leave`(754/761)：直接操作 NIC 的 L2 组播地址过滤表（`01:00:5e` + IP 低 23 位映射，`mt_mcast_ip_to_mac` inline 提供换算）。
- `mt_mcast_restore`(768)：端口 reset（如 VF 重建）后把全部成员关系重放。

---

## `mt_flow.c` / `mt_flow.h`

**一句话**：NIC 硬件流表（rte_flow）封装——把匹配 dip/dport（或 raw 字节）的报文精确引到指定 RX 队列，是"会话独占队列"的机制。

- `mt_rx_flow_create(impl, port, q, flow)`：两种组模式——
  - `rte_rx_flow_create`(74)：标准 eth/ipv4(+udp) item 匹配（dip、dst_port、可选 sip），action 为 `RTE_FLOW_ACTION_TYPE_QUEUE`；
  - `rte_rx_flow_create_raw`(19)：raw spec/mask 字节模式匹配（用于需要匹配 RTP 头字段等更细粒度分流，或 PMD 不支持 ipv4 item 时）。
- 返回 `mt_rx_flow_rsp`（含实际队列/规则句柄），`mt_rx_flow_free`(319) 删除规则；`mt_flow_init/uinit`(341/326) 管理每端口 flow 上下文。
- 使用方：`datapath/mt_queue.c` 的专用队列与会话 `mt_rxq_flow`；创建失败时上层回退到共享队列（RSS/CSQ）软件匹配。
- AF_XDP 场景不走 rte_flow，而是 eBPF 程序（`tools/ebpf/` + manager 经 `mt_instance_add_flow` 登记，详见 `../AF_XDP_EBPF.md`）。

---

## `mt_socket.c` / `mt_socket.h`

**一句话**：内核侧网络配置的辅助工具集——用 ioctl/`AF_PACKET` 读写接口地址与 MAC，给 kernel-socket 数据面挂 BPF 过滤器，加入组播组。

- 接口配置（`AF_INET + SOCK_DGRAM` ioctl）：`mt_socket_get/set_if_ip`(12/47)、`get_if_gateway`(86)、`get_if_mac`(119)、`set_if_up`(143)、`get_numa`(174，读 sysfs)。kernel 口 init 时校验/配置用。
- `mt_socket_get_mac`(294)：`AF_PACKET` raw socket 上对目标 IP 发 dummy 包触发 ARP，再等 reply 拿 MAC——比 DPDK 路径的 `mt_arp_get_mac` 更省事，给 kernel/af_xdp 口使用。
- kernel socket 数据面配套：`mt_socket_add_flow/remove_flow`(353/407)——给 UDP socket 挂 `SO_ATTACH_FILTER`（cBPF：UDP dst port + dip 匹配），使内核旁路多余流量；`mt_socket_fd_join_multicast`(424) / `get_multicast_fd`(452)：`IP_ADD_MEMBERSHIP` 组播加入。消费方是 `datapath/mt_dp_socket.c`。
- Windows 分支（475 起）全部为 no-op/`-ENOTSUP` stub。

---

## `mt_tap.c` / `mt_tap.h`

**一句话**：**Windows 专属**（`MTL_HAS_TAP`）——把 CNI 分不出来的报文桥进 TAP-Windows6/Wintun 虚拟网卡，让系统协议栈和 Wireshark 正常看到控制流量。

- 条件：`meson.build` 仅在 `is_windows && enable_tap` 时把 `mt_tap.c` 加入编译；否则 `mt_tap.h` 提供 inline stub。
- `mt_tap_init`(883)：枚举 `\\\\.\\Global\\*.tap` 设备句柄，`ST_IOCTL_GET_TAPMAC/VER/MTU/INFO` 取虚拟网卡属性（`USERMODEDEVICEDIR`，`TAP_WIN_DRIVERNAME "TAP-Windows Adapter"`，也支持 Wintun）；每端口一个 `tap_rt_context`（mempool + 双向 `overlapped_io`）。
- `mt_tap_handle`(861)：被 CNI 周期调用——读 TAP 上内核发包（写回线上 `cni_burst_from_kernel` 语义），写 TAP 下发给内核（收包转发）。

---

## `mt_pcap.c` / `mt_pcap.h`

**一句话**：基于 DPDK `rte_pcapng` 的抓包落盘——把端口/队列上的 mbuf 批量写成 pcapng 文件。

- `mt_pcap_open(impl, port, fd)`(25)：fd 由调用方准备（会话 `mt_rx_pcap.file_name`、CNI dump、USDT 探针路径），内部建专用 mempool（`rte_pcapng_mbuf_size`，512 对象）；`mt_pcap_dump`(65+) 拷贝 mbuf 写入（可带时间戳）；`mt_pcap_close`(12) 关 pcapng 并释放池（成功时 fd 由其关闭）。
- 时间戳依赖 **MTL 的 DPDK 补丁** `pcapng: add user timestamp support`（`patches/`）：`MTL_DPDK_HAS_PCAPNG_TS` → `rte_pcapng_copy_ts()`，让 dump 里的报文带上 NIC 硬件/PTP 时间；头文件注释明确上游采用了另一种接口形态。
- 无补丁的构建（`#else`）：三个入口降级为 `-ENOTSUP` inline stub——`lib/src/meson.build` 恒编译 `mt_pcap.c`，但功能由宏决定。

---

## 组内协作

```text
mt_dev/datapath ──RX 专用队列──► mt_cni（分类）
   mt_cni ──► mt_arp / mt_dhcp / mt_mcast / mt_ptp（协议处理）
   mt_cni ──► mt_pcap（dump） / mt_tap 或 mt_dp_socket（桥内核）
   mt_cni ──► mt_csq ◄── datapath/mt_queue（共享队列回退）
会话建立 ──► mt_flow（硬件引流）；kernel 口 ──► mt_socket（BPF/组播/ioctl）
```
