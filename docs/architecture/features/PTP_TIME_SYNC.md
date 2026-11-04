# MTL 时钟同步（PTP）实现分析

> 分析对象：`lib/src/mt_ptp.c` / `lib/src/mt_ptp.h`、`lib/src/mt_main.{c,h}`、`lib/src/dev/mt_dev.c`、`lib/src/mt_util.c`、`include/mtl_api.h`。基于 `feat/rk3588-apps` 分支。

## 1. 总览

MTL 内置一个软件 PTP 从时钟（slave-only，IEEE 1588v2，E2E delay request-response 机制，不实现 BMCA/PDelay），每个 port 一个实例 `mt_ptp_impl`（`impl->ptp[port]`）。它把 NIC 的 PHC（物理硬件时钟，PHC = PTP Hardware Clock）同步到上游 grandmaster，再可选地把 PHC 同步到系统时钟（内置 phc2sys）。整个数据面的时间基准（帧 pacing、RTP 时间戳、vsync）都从这条时间链上取值。

```
grandmaster (ptp4l/交换机)
   │  SYNC/FOLLOW_UP + DELAY_RESP  (L2 或 L4/UDP 组播 224.0.1.129)
   ▼
mt_ptp.c 从时钟 ──调整──▶ NIC PHC (rte_eth_timesync_adjust_time/_freq)
   │                          │
   │  phc2sys(可选)           │ ptp_get_time_fn
   ▼                          ▼
CLOCK_REALTIME (系统时钟)   mt_get_ptp_time() ──▶ pacing / RTP / vsync
```

## 2. 时间源选择（mt_dev.c:2527-2537）

每个 port 的 `inf->ptp_get_time_fn` 在 dev 初始化时按优先级选定：

| 优先级 | 条件 | 实现 | 时间域 |
|---|---|---|---|
| 1 | `MTL_FLAG_PTP_SOURCE_TSC` | `ptp_from_tsc`：`real_time_base + tsc - tsc_time_base`（dev 启动时用实时时间 + TSC 锚定的软件时钟） | TSC 近似 TAI |
| 2 | 用户设置 `ptp_get_time_fn` | `ptp_from_user`：直接回调用户 | 用户定义 |
| 3 | 默认 | `ptp_from_real_time` = `mt_get_real_time()`（系统 CLOCK_REALTIME） | 系统实时时间 |

之后，若启用了内置 PTP（`mt_user_ptp_service()`，即 `MTL_FLAG_PTP_ENABLE`）且未用 TSC/用户源，首条 ANNOUNCE 到达时 `ptp_parse_announce()` 会把 `ptp_get_time_fn` 切换为 `ptp_from_eth`（读 PHC + 系数修正）。注意：`ptp_parse_announce` 只在 `master_initialized` 第一次置位时切换，此后不再回退。

## 3. 内置 PTP 从时钟协议（mt_ptp.c）

### 3.1 收包路径

- **CNI 路径**：若 port 有 CNI 且驱动在数据面处理组播，依赖 CNI 收到组播包（`mt_mcast_join` 加入 `224.0.1.129` + L2 `01:1b:19:00:00:00`）。
- **独立 socket 队列路径**：否则通过 `mt_rxq_get` 创建两个 force-socket RX 队列（UDP 319 event / 320 gen），注册 tasklet `ptp_rxq_tasklet_handler` 挂在 `main_sch` 上轮询。每 burst 最多 4 包。
- 收包后进 `mt_ptp_parse()`：非 ANNOUNCE 报文必须来自已记录的 master（`source_port_identity` 匹配），否则丢弃 —— 即**不跑 BMCA，只认第一个 ANNOUNCE 的 master**。

### 3.2 消息处理与 t1–t4 测量

四时间戳法（SLAVE 视角）：

| 时间戳 | 含义 | 来源 |
|---|---|---|
| t1 | master 发 SYNC 的精确时间 | FOLLOW_UP 的 `precise_origin_timestamp` + `correction_field` |
| t2 | 本端收到 SYNC | `rte_eth_timesync_read_rx_timestamp`（硬件 RX 时间戳，`m->timesync` 索引） |
| t3 | 本端发 DELAY_REQ 的出口时间 | `rte_eth_timesync_read_tx_timestamp`（异步 alarm 轮询读取） |
| t4 | master 收到 DELAY_REQ | DELAY_RESP 的 `receive_timestamp` − `correction_field` |

流程：

1. **SYNC**（`ptp_parse_sync`）：清空 t1–t4、`result_claimed`，记录 t2。同时设置监控 alarm（`ptp_sync_timeout_handler`，周期为预期结果周期的一半，最小 100ms）——若一个周期内没凑齐 t1–t4，用历史均值 `ptp_sync_expect_result()` 兜底修正。
2. **FOLLOW_UP**（`ptp_parse_follow_up`）：seq 匹配后记 t1，并立即触发 `ptp_delay_req_task()` 发 DELAY_REQ（`MT_PTP_DELAY_REQ_US`=50us 延迟，多 port 依次错开 10us；`MT_PTP_USE_TX_TIMER` 开启时走 alarm，否则直接在 tasklet 里发）。
3. **DELAY_REQ 发送**：mbuf 打 `RTE_MBUF_F_TX_IEEE1588_TMST`，经 `mt_sys_queue_tx_burst` 发出。**t3 获取是异步的**：置 `t3_pending` + seq 守卫，alarm 每 200us（`MT_PTP_DELAY_REQ_TX_TS_RETRY_US`）轮询 TX 时间戳，2ms（`MT_PTP_DELAY_REQ_TX_TS_DEADLINE_US`）超时后放弃本周期并清空部分结果。有效 t3 必须满足 `tx_ns > t2`（丢弃被上一周期锁存的陈旧 PHC 槽位值）。注释解释了为什么：PHY 要等包真正出门（队列积压/Qbv 最多 ~1.2ms）才出时间戳，忙等会占住 tasklet。
4. **DELAY_RESP**（`ptp_parse_delay_resp`）：校验 `requesting_port_identity` 与 seq，记 t4，然后 `ptp_try_complete_result()`。
5. **结果计算**：t3（alarm 线程）和 t4（RX tasklet）都可能先凑齐四元组，`result_claimed`（`rte_atomic32_test_and_set`）保证**每个周期只算一次**。

### 3.3 偏移计算与鲁棒性（`ptp_parse_result`）

- 经典公式：`delta = ((t4−t3) − (t2−t1)) / 2`，`path_delay = ((t2−t1) + (t4−t3)) / 2`。
- **无硬件 timesync 时**（`no_timesync`，如 VF / AF_XDP / 内核 socket 后端）：所有 rte_eth_timesync_* 调用退化为 TSC + `no_timesync_delta`，且开启 `calibrate_t2_t3`——用滚动均值 `expect_t2_t1/t4_t3_delta_avg` 校准超过 ±1us 的离谱 t2/t3 差（连续 20 次超差则重置学习）。
- **异常结果过滤**：与历史均值 `expect_result_avg`（仅对 |delta|<30us 的样本累计，10 个一批）比较，超预期（均值×min(err+2,5)，下限 100us）则清空本轮、计数错误，连续 >10 次错误重置全部统计。
- **锁定判定**：|delta| 连续 100+ 次样本最大值 < 100ns → `locked = true`。`mt_ptp_is_locked()` 供外部查询。
- 每 10s（`MT_PTP_STAT_INTERVAL_S`）`ptp_stat()` dump delta/path_delay/correct_delta 的 min/max/avg、sync 次数、超时与错误计数。

### 3.4 对 PHC 的调整（`ptp_adjust_delta`）

两种伺服：

- **默认（步进式）**：`ptp_timesync_adjust_time(ptp, delta)` 直接步进 PHC（无 timesync 时只累加 `no_timesync_delta`）。若 DPDK 支持 `rte_eth_timesync_adjust_freq` 且 `phc2sys_active`，走下面的 PI 伺服调 PHC。
- **PI 伺服（`MTL_FLAG_PTP_PI`，仅 PF）**：`pi_sample()` 复刻 linuxptp PI servo——前 3 个样本只积累 drift，第 4 个样本 JUMP（一次性步进），之后 LOCKED 用 `ppb = 0.3*offset + drift` 调频（`ptp_timesync_adjust_freq`），失败则回退步进。

另有**软件漂移系数**（`coefficient`，双独立机制）：PHC 只在每秒被调整一次，期间 PHC 自由漂移。`ptp_correct_ts()` 用 `coefficient` 把原始 PHC 读数外插到"真实 PTP 时间"：

- 非 PI 模式：`ptp_calculate_coefficient()` 直接测量 `（PHC+delta 的增长率 / PHC 原始增长率）`，每 10 个样本去掉最大最小后取均值。
- PI 模式：`ptp_update_coefficient()` 用 kp/ki（默认 `MT_PTP_DEFAULT_KP/KI`，可由 `mtl_init_params.kp/ki` 覆盖）对 correct_delta 做 PI 微调，单步限幅 ±1e-7；L4 模式下 offset ÷4（同步间隔 0.25s 的归一）。

`mt_mbuf_time_stamp()`（RX 帧时间戳）对带硬件时间戳的 mbuf 同样过 `ptp_correct_ts`——注释明确**不再叠加软件 PTP offset**：timesync 路径下 `adjust_time` 已经步进 PHC；无 timesync 路径下叠加 TSC 域 offset 会导致 ST 2110-21 时间戳全错，因此直接信任 NIC 时钟原始值。

### 3.5 内置 phc2sys（`phc2sys_adjust`）

`MTL_FLAG_PTP_PHC2SYS` 且为主 port 时启用：10 次成对采样 `CLOCK_REALTIME` 与 PHC（取读数间隔最短的一对求 offset），喂给独立的 `mt_pi_servo`，JUMP 调系统时钟偏移（`clock_adjtime(CLOCK_REALTIME, ADJ_SETOFFSET)`），LOCKED 调频（`ADJ_FREQUENCY`，ppb→scaled ppm ×65.536，并先尝试 `ADJ_TICK` 补偿）。系统时钟锁定判定为连续 100 次 |offset| < 300ns。需要 `cap_sys_time`（失败日志会提示 `setcap 'cap_sys_time+ep'`）。Windows 用 `SetSystemTimePrecise` 等价实现。

### 3.6 用户时间源同步（`ptp_sync_from_user`）

若 NIC 有 offload 时间戳能力但未启用内置 PTP 服务：PTP 实例退化为"把 PHC 对齐到用户 `ptp_get_time_fn`"的随动器——每 `MT_PTP_TP_SYNC_MS` 计算目标时间与 PHC 之差并 `adjust_time`（只调时钟本身，`mbuf_hw_time_stamp` 依赖此约定），同时学习静态 `expect_result_avg`，直接置 `connected/locked=true`。

## 4. PTP 时间的消费方

| 消费点 | 用途 |
|---|---|
| `mtl_ptp_read_time()`（公共 API） | 优先读 10ms 缓存（`ptp_usync` + TSC 外插），避免每次 MMIO 读 PHC；缓存过期才真正 `mt_get_ptp_time`。ST 2110-21 RX 时间戳、`mt_mbuf_time_stamp` 无硬件时间戳回退路径使用 |
| `mtl_ptp_read_time_raw()` | 直接读 PHC，无缓存 |
| `st_vsync_calculate()`（mt_util.c:929） | 用 PTP 时间计算下一个帧对齐 epoch（`ptp_time / frame_time + 1`），换算成 TSC 目标 `next_epoch_tsc` 供 TX/RX video session 的 vsync 事件用——PTP 域计算、TSC 域等待 |
| `mt_ptp_wait_stable()` | TX video session 启动前等待前 5 个 delta 结果（最长 180s），保证 pacing 起点在时钟锁定后 |
| TSN pacing 检查（mt_dev.c:1602） | launch time 与 PHC 比较，要求内置 PTP 服务的 PHC 已同步，否则拒绝 `ST21_TX_PACING_WAY_TSN` |
| `ptp_sync_notify` 回调 | 每次 DELAY_RESP 后向应用上报 `master_utc_offset` 与本次 delta（仅主 port） |

## 5. 对外 API 与配置项（include/mtl_api.h）

- `MTL_FLAG_PTP_ENABLE`（bit1）：启用内置 PTP 从时钟。
- `MTL_FLAG_PTP_PI`（bit9）/ `kp` / `ki`：PI 伺服及增益。
- `MTL_FLAG_PTP_UNICAST_ADDR`（bit35）：DELAY_REQ 用单播地址（master 地址从 ANNOUNCE 的 clock_identity 推导 MAC：`id[0:3] + id[5:8]`）；默认 L2/L4 组播地址（`01:1b:19:00:00:00` / `01:00:5e:00:01:81`）。
- `MTL_FLAG_PTP_SOURCE_TSC`（bit42）：强制 TSC 作为 PTP 源（无需 NIC timesync，如 RK3588 无 PHC 网卡时的软件替代）。
- `MTL_FLAG_ENABLE_HW_TIMESTAMP`（bit8）：仅开硬件时间戳不开 PTP 协议。
- `ptp_get_time_fn`：用户提供时间源；`ptp_sync_notify`：同步结果通知。
- `mtl_ptp_read_time` / `mtl_ptp_read_time_raw`：读时间。

## 6. 特性与限制小结

1. **slave-only、无 BMCA**：认第一个 ANNOUNCE 的 master，master 变更需重启（`master_initialized` 不会重置）。
2. **单 master 过滤**：其它 PTP 节点的报文全部丢弃，`ptp->port` 维度隔离；多 port 时 DELAY_REQ 的 seq 以 `0x1000*port` 起始避免串扰，DELAY_REQ 发送按 port 错开 10us。
3. **L2 与 L4 均支持**（`MT_PTP_L2/L4`），L4 走 UDP 319/320；DELAY_REQ 的 VLAN 路径未实现（`todo for vlan`）。
4. **PDelay（802.1AS）未实现**：报文类型里有解析分支但为空。
5. `ptp_timesync_lock` 目前是空实现（todo），timesync 寄存器读写在多线程间无锁保护，靠 tasklet 单线程 + alarm 线程的因果设计（seq 守卫、t3>t2 校验、result_claimed）规避。
6. 无硬件 timesync 时整条链路退化为 TSC 域软件时钟，精度取决于 TSC 频率校准与网络对称性；这正是 RK3588 等 ARM 平台（无 E810/IGC PHC）走 `MTL_FLAG_PTP_SOURCE_TSC` 或用户时间源的原因。
7. 时钟域要点：PHC 读数为 TAI 域（含 master UTC offset 用于显示）；系统时钟为 UTC；TSC 为单调相对域。三者转换点集中在 `ptp_get_time_fn` 选择器、phc2sys 和 `real_time_base/tsc_time_base` 锚点。
