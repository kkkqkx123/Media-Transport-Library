# lib/src 核心模块：实例与调度

> 源文件：`mt_main.c/h`、`mt_sch.c/h`、`mt_admin.c/h`、`mt_config.c/h`、`mt_instance.c/h`

---

## `mt_main.c` / `mt_main.h`

**一句话**：整个库的入口和"聚合根"。实现 `mtl_init` 等生命周期 API，`mt_main.h` 里的 `struct mtl_main_impl` 持有其余所有模块的状态。

### mt_main.h（2032 行）

- **`struct mtl_main_impl`**（约 1168 行起）——实例主体，按上下文分组持有：
  - `inf[MTL_PORT_MAX]`（`mt_interface`，端口配置）、`user_para`（`mtl_init_params`）、`var_para`（`mt_var_params`，运行时派生值）、`kport_info`；
  - 数据路径：`flow[]`（rte_flow）、`dp[]`（datapath 后端）、`rss[]`/`srss[]`、`rsq[]`/`tsq[]`（共享收/发队列）；
  - 服务：`stat_mgr`、`admin`、`cni`、`ptp[]`、`arp[]`、`mcast[]`、`dhcp[]`、`sch_mgr`、`plugin_mgr`、`map_mgr`（IOVA 映射）、`u_info`（pid/uid）、`instance_fd`（MtlManager 连接）；
  - 状态原子：`instance_started` / `instance_in_reset` / `instance_aborted`；`main_sch` 为系统级 scheduler。
- **全局常量**：`MT_MAX_SCH_NUM`（18，AGENTS.md 提到的调度器上限）、`MT_MBUF_CACHE_SIZE`(128)、`MT_MAX_RL_ITEMS`(128)、`MT_ARP_ENTRY_MAX`(60)、`MT_MCAST_GROUP_MAX`(60)、`MT_DMA_MAX_SESSIONS`(16) 等。
- **大量 inline 访问器**：`mt_socket_id()`、`mt_user_*()`（各 MTL_FLAG_* 判定）、`mt_num_ports()`、`mt_if_*()`（端口能力查询）等——这是各 `mt_*.c` 共享实现细节的方式。
- `MT_TAILQ_*` 宏桥接 RTE 与系统 `sys/queue.h`。

### mt_main.c（1385 行）

生命周期主链（对应知识库 §6）：

| 函数 | 行 | 作用 |
|------|----|------|
| `mtl_init` | 377–622 | 全库入口：`mt_user_params_check` 参数校验（PMD 类型/IP/端口组合）→ `mt_main_create`（TSC 校准、hugepage 信息、NUMA）→ 依次初始化 config、sch_mgr、instance（连 MtlManager）、dev/queue、cni、arp/mcast/dhcp/ptp、dma、stat、admin → 非 `MANUAL_START` 时直接启动调度器 |
| `mtl_uninit` | 624–660 | 反序拆除 |
| `mtl_start` / `mtl_stop` | 662–694 | `_mt_start/_mt_stop`：启停全部 scheduler（`mt_sch_start_all/stop_all`） |
| `mtl_abort` | 743 | 异常中断路径（应用 ctrl-c 场景） |
| `mt_dst_ip_mac` | 38 | 组装目的 IP/MAC（内部调 `mt_arp_get_mac`） |

同时实现了大量 `include/mtl_api.h` 的公开 API 落地点：

- lcore 借用：`mtl_get_lcore` / `mtl_put_lcore` / `mtl_bind_to_lcore`（转调 `mt_sch.c` 的仲裁逻辑）；
- 外部内存：`mtl_dma_map` / `mtl_dma_unmap` / `mtl_dma_mem_alloc/free/iova`（配合 `mt_dma.c` 的 `mt_map_*` 表，把非 hugepage 内存映射进 IOVA 供 NIC 访问）；
- `mtl_hp_free` / `mtl_hp_virt2iova` / `mtl_page_size`；
- 用户 DMA：`mtl_udma_create/free/copy/fill/submit/completed`（应用直接借 dmadev 通道）；
- 时间：`mtl_ptp_read_time(_raw)`（转调 `mt_ptp.c`）；`mt_tsc_mono_pair` 提供 TSC↔monotonic 换算；
- 信息：`mtl_get_fix_info` / `mtl_get_var_info` / `st_get_var_info`、`mtl_port_ip_info`、`mtl_rss_mode_get`、`mtl_iova_mode_get`、`mtl_get_simd_level`、`mtl_get_numa_id`、`mtl_pmd_is_dpdk_based`；
- 杂项：`mtl_thread_setname`、`mtl_sleep_us/mtl_delay_us`、`mtl_para_sip_set/gateway_set/netmask_set/port_set/dma_port_set`（init 前改写参数）、`mtl_sch_enable_sleep` / `mtl_sch_set_sleep_us`（转 `mt_sch.c`）；
- `mt_sessions_time_measure`：会话耗时测量开关的实现点（与 USDT `sys:sessions_time_measure` 探针联动）。

---

## `mt_sch.c` / `mt_sch.h`

**一句话**：协作式 tasklet 调度引擎——库的心脏。管理 ≤18 个 scheduler，负责 lcore 分配与多进程仲裁、1080p 当量配额、睡眠节能。

### 结构

- `struct mt_sch_mgr`：`sch[MT_MAX_SCH_NUM]` 数组 + `lcore[]`（`mt_lcore_mgr`，POSIX 共享内存 `mt_lcore_shm` 做跨进程记账）+ 文件锁（`MT_FLOCK_PATH`，见 `mt_platform.h`）。
- `struct mtl_sch_impl`（在 `mt_main.h` 定义）：一个调度器实例 = 1 个 lcore（或 1 个 pthread，`MTL_FLAG_TASKLET_THREAD`），内含 tasklet 链表和各类型会话子管理器（`tx_video_mgr`/`rx_video_mgr`/audio/anc…）、`data_quota_mbs_total` 配额、sleep 状态。
- `mt_sch.h` 提供 `mt_sch_get_mgr/instance/is_active/started/has_busy/cpu_busy` 等 inline 访问器，供 `mt_admin.c` 等使用。

### 关键逻辑

| 功能 | 函数 | 说明 |
|------|------|------|
| 轮询主循环 | `sch_tasklet_func`(153) / `sch_tasklet_lcore`(242) | 依次调用所有 tasklet 的 `func`（>0 有活、0 空闲），统计耗时（`sch_tasklet_time_measure`）；lcore 模式下为绑核忙轮询 |
| 节能睡眠 | `sch_tasklet_sleep`(62) + `sch_sleep_alarm_handler` | 空转超阈值且 `mtl_sch_enable_sleep` 打开时，cond timedwait + `rte_eal_alarm` 唤醒；`MTL_FLAG_TASKLET_THREAD`/容器场景 |
| NUMA 亲和 | `sch_set_local_mempolicy`(126) | 调度线程运行期间把 mempolicy 收紧到本 socket（呼应"NUMA mismatch 翻倍 DMA 延迟"） |
| lcore 仲裁 | `mt_sch_get_lcore`(699) / `mt_sch_put_lcore`(791) | 三级来源：本地 shm 位图 → 文件锁保护 → 向 MtlManager 申请（`mt_instance_get_lcore`）；`lcore_shm_clean_auto_pid`(1285)/`lcore_shm_clean_id` 清理已死进程残留；`mtl_lcore_shm_clean`/`mtl_lcore_shm_print` 为公开维护 API |
| 配额 | `mt_sch_add_quota`(1052) / `mt_sch_put`(1076) / `sch_is_capable`(429) | 会话按 1080p-MB/s 当量占额度，`mt_sch_get_by_socket` 找同 socket 有余量的调度器（`mt_admin.c` 迁移时用） |
| 生命周期 | `mt_sch_mrg_init/uinit`(966/1017)、`mt_sch_start_all/stop_all`(1185/1208) | `mtl_init` 建全部、`mtl_start` 统一启动 |
| 公开 API | `mtl_sch_create/free/start/stop` + `mtl_sch_register_tasklet/unregister_tasklet`(1378/923/871) | 应用自定义任务挂进调度器（`include/mtl_sch_api.h`） |
| 统计 | `sch_stat`(450) | 周期打印各 tasklet calls/time/cycle 分布（注册到 `mt_stat.c`） |

---

## `mt_admin.c` / `mt_admin.h`

**一句话**：6 秒周期的后台管理线程——统计各视频会话 CPU busy，过载时把"忙"的 TX/RX 视频会话整体迁移到同 socket 的另一调度器。

- `mt_admin_init`(376)：pthread `mtl_admin` + `rte_eal_alarm`（`period_us = 6s`）驱动 cond 唤醒，循环跑 `admin_func`(332)。
- `admin_func` 每轮：
  1. `mt_update_admin_port_stats`（端口级统计快照，见 `dev/mt_dev.c`）；
  2. `admin_cal_cpu_busy`(16)：遍历所有已启动调度器的 TX/RX 视频会话，调 `tx/rx_video_session_cal_cpu_busy` 计算 busy 度；
  3. 若 `MTL_FLAG_TX_VIDEO_MIGRATE`（`mt_user_tx_video_migrate`）：`admin_tx_video_migrate`(143) 找最忙会话 → `mt_sch_get_by_socket` 选目标 → `tx_video_migrate_to`(101)：持双方 `tx_video_mgr_mutex`，从旧 mgr 摘槽、`st_tx_video_session_migrate` 重建队列/RL 等资源、挂入新槽并更新 handle→sch 指针，旧 sched `mt_sch_put` 归还配额；单会话独占调度器（quota ≥ total）时跳过；
  4. 未发生 TX 迁移且开了 RX 迁移旗标：`admin_rx_video_migrate`(260) 同构流程（仅对 `rx_video_session_can_migrate` 的会话，slot 模式等可迁移形态）；
  5. 迁移成功则 `admin_clear_cpu_busy` 重置基线。**每周期至多迁移一个会话**。
- `mt_admin.h` 只暴露 `mt_admin_init/uinit`。

---

## `mt_config.c` / `mt_config.h`

**一句话**：实例启动时读取 JSON 配置文件 `kahawai.json`（或环境变量 `KAHAWAI_CFG_PATH`），当前唯一功能是把 `plugins` 数组注册为编解码插件。

- `mt_config_init`(52) → `config_parse_json`：`json_object_from_file`（json-c）→ `config_parse_plugins`(9)：对每个 `{"enabled":true,"name":…,"path":…}` 项调 `st_plugin_register(impl, path)`（实现在 `st2110/pipeline/st_plugin.c`），供 ST22/转换插件 dlopen。
- 文件缺失只 `warn` 不报错——配置文件是可选的。
- `mt_config_uinit` 为空实现。

---

## `mt_instance.c` / `mt_instance.h`

**一句话**：库内嵌的 **MtlManager 客户端**——AF_UNIX socket IPC，把 lcore/队列/flow/XDP map 等跨进程共享资源的仲裁委托给 `manager/` 里的守护进程。

- 协议复用 `manager/mtl_mproto.h`：固定长 `mtl_message_t`，头带 `MTL_MANAGER_MAGIC`；`instance_send_and_receive_message`(15) 发送并校验响应类型。
- `mt_instance_init`(188)：`connect(MTL_MANAGER_SOCK_PATH)` → 发 `MTL_MSG_TYPE_REGISTER`（pid/uid）；**连接失败时仅 warn，退回单实例模式**（各调用点自行判 `impl->instance_fd < 0`）。
- 资源请求接口：

| 接口 | 消息 | 用途 |
|------|------|------|
| `mt_instance_get_lcore/put_lcore` | GET/PUT_LCORE | 多进程 lcore 互斥（`mt_sch.c` 的后端） |
| `mt_instance_request_xsks_map_fd` | IF_XSK_MAP_FD | 向 manager 要 AF_XDP `xsks_map` 的 fd（**SCM_RIGHTS 传递**，配合共享 eBPF 程序，详见 `../AF_XDP_EBPF.md`） |
| `mt_instance_update_udp_dp_filter` | ADD/DEL_UDP_DP_FILTER | 在 manager 的 eBPF 里增删某网卡上的 UDP 目的端口过滤（内核旁路白名单） |
| `mt_instance_get_queue/put_queue` | IF_GET/PUT_QUEUE | 按 ifindex 分配网卡 RX 队列号 |
| `mt_instance_add_flow/del_flow` | ADD/DEL_FLOW | 在 manager 侧登记流规则（XDP 分流场景） |

- Windows（或 `MTL_MANAGER_SOCK_PATH` 不可用的构建）：345 行文件后半是同名 no-op/stub 版本，返回固定成功或 `-ENOTSUP`，保持链接兼容。

---

## 组内依赖小结

```text
mt_main.c ── 创建/编排 ──► mt_config.c（插件）
           ├─► mt_sch.c（18 个调度器 + lcore 仲裁）──► mt_instance.c（MtlManager）
           ├─► mt_admin.c（读 mt_sch.h inline 状态；迁移调 st2110 会话内部 API）
           └─► include/mtl_api.h 公开 API 的落地层
```
