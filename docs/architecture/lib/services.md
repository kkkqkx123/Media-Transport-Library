# lib/src 支撑服务：PTP / RTCP / DMA / 统计 / 日志 / 工具

> 源文件：`mt_ptp.c/h`、`mt_rtcp.c/h`、`mt_dma.c/h`、`mt_stat.c/h`、`mt_log.c/h`、`mt_util.c/h`、`mt_usdt.h`、`mt_usdt_provider.d`

---

## `mt_ptp.c` / `mt_ptp.h`

**一句话**：内置 IEEE 1588 PTP 客户端——从报文解析、Servo 收敛到系统时钟校正的全链路实现，ST 2110 时间戳（RTP 里的 media clock）的地基。

### 协议面（`mt_ptp.h`）

- UDP 端口 319（event）/ 320（general），消息类型 `PTP_SYNC/DELAY_REQ/PDELAY_*/FOLLOW_UP/DELAY_RESP/ANNOUNCE/SIGNALING/MANAGEMENT`；
- packed 报文结构：`mt_ptp_header`、`mt_ptp_sync_msg`、`mt_ptp_follow_up_msg`、`mt_ptp_announce_msg`（含 grandmaster clock quality/priority）、`mt_ptp_delay_resp_msg`、`mt_ptp_tmstamp`；
- `mt_ptp_parse(ptp, hdr, vlan, mode, timesync, ipv4_hdr)` 是总入口，由 CNI 分发或专用 PTP RX 队列 tasklet 调用。

### 状态机与伺服（`mt_ptp.c`，1680 行）

| 环节 | 函数 | 说明 |
|------|------|------|
| 报文解析 | `ptp_parse_sync`(950) / `ptp_parse_follow_up`(1008) / `ptp_parse_announce`(1032) / `ptp_parse_delay_resp`(1073) | 支持 one-step / two-step（correction field 累加）、VLAN、multicast 与 unicast 协商模式 |
| 主动延迟测量 | `ptp_delay_req_task`(844) / `ptp_delay_req_handler` | 周期发 Delay_Req；TX 硬件时间戳（t3）**异步读取**：200us 重试间隔、2000us 截止（头文件注释：覆盖 Qbv 出口排队最坏情形，同时限制 PMD busy-wait 次数） |
| 结果判定 | `ptp_parse_result`(626) / `ptp_sync_expect_result` / `ptp_try_complete_result` | 凑齐 t1–t4 后计算 offset |
| Servo | `ptp_calculate_coefficient`(454) / `ptp_update_coefficient`(445) / `ptp_adjust_delta`(477) | 比例-积分式系数收敛；小偏差走频率微调（slew），大偏差直接步进（step） |
| 时钟落地 | `ptp_adj_system_clock_time`(163) / `ptp_adj_system_clock_freq`(207) | Linux 用 `clock_adjtime`（PHC/系统），Windows 用 `SetSystemTimeAdjustment`（需 `obtain_systime_privileges()` 提权） |
| 监控 | `ptp_monitor_handler`(598) / `ptp_sync_timeout_handler`(611) | 1s 巡检 announce/sync 是否还活着，超时清 locked |
| 用户时间注入 | `ptp_sync_from_user`(1123) | `MTL_FLAG_PTP_SYNC_UNICAST`/用户提供时间路径：把外部给的 tai 时间同步进内部基准（`impl->ptp_usync`） |
| RX 通道 | `ptp_rxq_tasklet_handler`(1250) | 有专用 PTP flow 队列时走 tasklet（数据面），否则由 CNI 转 `mt_ptp_parse` |

- 实例化：`mt_ptp_init`(1561) **只探测** MTL_PORT_P 或具备 offload timestamp 能力的端口；`mt_stat_register(ptp_stat)` 10s 统计；USDT `ptp:ptp_msg/ptp_result` 探针输出每阶段时间戳与收敛结果。
- 状态查询：`mt_ptp_is_active/is_locked/is_connected`、`mt_ptp_internal_time`；公共 API `mtl_ptp_read_time(_raw)` 在 `mt_main.c` 转发到此。

---

## `mt_rtcp.c` / `mt_rtcp.h`

**一句话**：MTL 自定义 RTCP NACK 重传机制——TX 端缓存已发 RTP 供重传，RX 端用 seq bitmap 检测丢包并回 NACK。（ST 2110-21 允许的重传路径，非标准 SMPTE 报文，name 字段校验 `"IMTL"`。）

### TX 侧

- `mt_rtcp_tx_create/free`：按 session ops（udp 头模板、ssrc、buffer 大小、payload 格式）建实例；RTP 缓存环 `TRT_` 前缀（`mt_u64_fifo` + 专属 mempool）。
- `mt_rtcp_tx_buffer_rtp_packets`(28)：发送后把 mbuf 引用挂入环（超出容量丢最旧）。
- `mt_rtcp_tx_parse_rtcp_packet`(194)：解析收到 NACK——校验 flags=0x80、ptype=204、name=`IMTL`、长度（4 种 drop reason 计数，`MT_RTCP_DROP_SAMPLE` 采样日志）；对每个 FCI（start/follow 位图，`mt_rtcp_fci`）调 `rtcp_tx_retransmit_rtp_packets`(68) 批量补发（≤`MT_RTCP_RETRANSMIT_BULK` 32），统计 succ/fail（nobuf/read/obsolete/burst）。

### RX 侧

- `mt_rtcp_rx_create/free` + `mt_rtcp_rx_parse_rtp_packet`(277)：seq bitmap（`seq_bitmap_size` 检测窗口 + `seq_skip_window` 乱序容忍）标记空洞，`rtp_seq_num_cmp`(14) 处理 16bit 回绕；
- `mt_rtcp_rx_send_nack_packet`(322)：按 `nacks_send_interval` 把空洞打包成 FCI 发回（≤256 FCI），统计 nack_sent/lost_detected。

两侧 `rtcp_tx_stat/rx_stat`(417/451) 注册到 `mt_stat` 周期输出。使用方是 st20 TX/RX 会话（session 层在流上多路复用 RTCP 端口）。

---

## `mt_dma.c` / `mt_dma.h`

**一句话**：DPDK dmadev（idxd / IOAT）设备管理 + IOVA 映射表——把大页拷贝/清零卸载到 DMA 引擎，并支撑外部内存的 `mtl_dma_map`。

### 设备与 lender 模型

- `mt_dma_init/uinit`(521/566)：探测 dmadev，`dma_copy_test`(133) 做可用性自检（IOVA 连续性验证），`dma_hw_start/stop` 管队列；
- **lender 共享**：一个物理 DMA 设备最多 `MT_DMA_MAX_SESSIONS`(16) 个 session 借用——`mt_dma_request_dev(req{nb_desc,max_shared,sch_idx,socket_id,drop_mbuf_cb})` 发放 `mtl_dma_lender_dev`（带各自描述符配额），`mt_dma_free_dev` 归还；
- 操作封装：`mt_dma_copy/fill/submit/completed`（+ `*_busy` inline 自旋版），供 RX 会话把 mbuf→framebuffer 拷贝卸载（知识库 §3）。

### mbuf 借用（异步释放）

- `mt_dma_borrow_mbuf/drop_mbuf`(484/508)：把待释放 mbuf 先入队，由 DMA 设备空闲时批量 `fill` 清零再 free，避免 CPU memset 抢占 RX 热路径；`MTL_DMA_BORROW_RING_PREFIX "DB_"`。
- 环实现二选一（编译宏 `MT_DMA_RTE_RING`，mt_main.h 定为 1）：`rte_ring` 或裸 u64 fifo（190–345 行两组分支）。

### IOVA 映射表（同文件前半）

- `mt_map_init/uinit/add/remove`(103/111/14/71)：`impl->map_mgr` 维护 VA↔IOVA 区间表，是 `mt_main.c mtl_dma_map/unmap/dma_mem_alloc`（外部内存/显存映射给 NIC/DMA 用）的底座。

### 条件编译

- 整块实现包在 `#if RTE_VERSION >= 21.11`（dmadev API 引入版本）内；旧 DPDK 走 582 行起的 stub（`mt_dma_full` 恒 false 等，上层自动退回 CPU memcpy）。

---

## `mt_stat.c` / `mt_stat.h`

**一句话**：统计打印的统一出口——注册制回调 + 专用线程周期 dump，避免各模块自己起定时器。

- `mt_stat_register/unregister(cb, priv, name)`(100/121)：各模块（sch/cni/ptp/dma/rtcp/dev…）挂入自己的 stat 回调；
- `mt_stat_init`(143)：pthread `mtl_stat` + `rte_eal_alarm` 每 `dump_period_us`（默认 `MT_STAT_INTERVAL_S_DEFAULT` 10s，可由实例参数覆盖）唤醒 `_stat_dump`(30) 顺序调用全部回调；
- 与数据面解耦：tasklet 内只更新计数器，格式化/打印发生在这个线程——符合两世界规则。

---

## `mt_log.c` / `mt_log.h`

**一句话**：日志设施——DPDK RTE_LOG 之上加实例前缀、可替换 printer、级别过滤与 USDT 旁路。

- `mt_log.h`：`MT_LOG` 宏统一走 `RTE_LOGTYPE_MTL`，支持自定义前缀 formatter（`mtl_set_log_prefix_formatter`，默认 pid+时间）与 printer 接管（`mtl_set_log_printer`）；`dbg/info/notice/warn/err/critical` + `*_once` 变体；**`dbg()` 只在 `DEBUG` 宏下编译进**——release 数据面无格式化开销。
- `mt_log.c`：`mt_set_log_global_level` / `mtl_set_log_level`（实例级，写回全局）；`mtl_openlog_stream` 重定向到 FILE*；`log_usdt_printer`——当 `sys:log_msg` USDT 探针 attach 时日志同时经 DTrace 输出（运行时打开 DEBUG 级而不重编译）。

---

## `mt_util.c` / `mt_util.h`

**一句话**：全库工具箱——内存池/位图/环/校验和/端口映射/pacing 训练缓存/proc 采样等公共例程的集合。

按主题分块（`mt_util.c` 1154 行）：

| 主题 | 代表接口 | 备注 |
|------|---------|------|
| 拷贝后端 | `mt_memcpy`（头文件 inline） | 显式选 libc `memcpy`（glibc 运行时挑 AVX-512/ERMS），注释记录 `rte_memcpy` 在 RX 帧写路径更慢——这是数据面拷贝的唯一入口 |
| 位图 | `mt_bitmap_test/test_and_set/test_and_unset` | RX slot 去重、seq 空洞检测的基础件 |
| 大内存环 | `mt_u64_fifo_*` | hugepage 上自研 FIFO（put/get/bulk/read_any），RTCP 缓存与 DMA 借用环的底层；`mt_fifo_mbuf_clean` 释放成员 |
| Mempool | `mt_mempool_create_by_ops/…/dump/free` | 默认 `stack` ops + socket 亲和（NUMA 铁律）；`recovery_idx` 命名唯一性配合它 |
| pacing 训练缓存 | `mt_pacing_train_pad/bps_result_add/search` | TX 训练结果按端口缓存复用（重启会话免重训） |
| 网络小工具 | `mt_is_multicast_ip/is_lan_ip`、`mt_ip_to_u32/u32_to_ip`、`mt_rf1071_check_sum`、`mt_softrss` | 组播判定、IP↔u32、IPv4 头校验和（IGMP/ARP 报文）、Toeplitz 软件 RSS 计算 |
| 端口映射 | `mt_build_port_map`、`mt_port_by_name`、`mt_port_logic2phy`、`mt_dpdk_afxdp/afpkt/kernel/native_afxdp_port2if` | session 逻辑口(s_port)↔物理口映射；各 PMD 端口名→接口名 |
| 报文构造/dump | `mt_build_pad`、`st_video_rtp_dump`、`mt_mbuf_dump(_hdr)`、`mt_mempool_dump`、`mt_eth_link_dump`、`mt_lcore_dump` | padding 包构造 + 调试导出 |
| 参数校验 | `st_tx_dest_info_check`、`st_rx_source_info_check`、`mt_ip_addr_check`、`mtl_*_port_check`（mt_main） | init 期边界检查 |
| 帧/时间 | `st_vsync_calculate`、`st_frame_trans_uinit` | vsync 周期与 epoch 计算（pipeline 帧调度用） |
| 系统采样 | `mt_user_info_init`（pid/comm）、`mt_read_cpu_usage/mt_calculate_cpu_usage`（/proc/stat）、`mt_file_exists`、`mt_sysfs_write_uint32`、`mt_run_cmd` | `mt_admin` 迁移判定和 instance 注册都依赖它 |
| 杂项 | `mt_udp_matched`（软件 flow 匹配）、`mt_random_port`、`mt_fd_set_nonbolck`/`mt_mkstemps`（Win 兼容 shim）、`mt_stat_u64_update` | |

ASAN 构建下 `mt_asan_check`/`mt_rte_free` 替换 `mt_mem.h` 里的直连版本，为 DPDK 分配加追踪。

---

## `mt_usdt.h` / `mt_usdt_provider.d`

**一句话**：USDT（Userspace Statically Defined Tracing）静态探针层——不发版、不重编译，运行时用 bpftrace/SystemTap 打开帧 dump、pcap 抓取、计时等深度观测。

- `mt_usdt_provider.d`（dtrace 输入，构建生成 `mt_usdt_provider.h`）定义的 provider：
  - **sys**：`log_msg`（实时日志旁路）、`tasklet_time_measure`、`sessions_time_measure`、`cni_pcap_dump`；
  - **ptp**：`ptp_msg`/`ptp_result`（同步各阶段时间戳与收敛）；
  - **st20/22/30/40 与 st20p/22p/30p/40p**：`tx_frame_next/done/dump`、`rx_frame_available/put/no_framebuffer/dump`、`rx_pcap_dump`、st22p 的 `tx_encode/rx_decode` 等——探针点在 `st2110/` 会话与 pipeline 文件里，本组根目录只负责宏设施；
- `mt_usdt.h`：`MTL_HAS_USDT` 时映射到 `DTRACE_PROBEn`，否则全部展开为空语句 + `*_ENABLED()` 恒 0——热路径以 `MT_USDT_*_ENABLED()` 短路，未 attach 时零成本。

---

## 组内协作

```text
mt_ptp ◄── mt_cni/mt_ptp_parse ── 媒体会话时间戳(st2110)
mt_rtcp ◄── st20 会话（RTP 缓存 + NACK）
mt_dma ◄── st RX 会话（拷贝卸载） + mt_main(mtl_dma_map 外部内存)
mt_stat ◄── 几乎所有模块（sch/cni/ptp/dma/rtcp/dev）
mt_log / mt_usdt ── 全库横切
mt_util ── 全库公共件
```
