# lib/src 平台抽象与工具头文件

> 源文件：`mt_mem.h`、`mt_platform.h`、`mt_header.h`、`mt_quirk.h`、`mt_simd.h`、`mt_handle_guard.h`、`meson.build`
>
> 本组没有 `.c` 对应物（除 `meson.build`），是其余所有 `mt_*.c` 共同仰赖的地基层。

---

## `mt_mem.h`（46 行）

**一句话**：全库内存分配的单一入口——把"控制面 malloc"和"数据面 hugepage 分配"在类型上分开，并给 ASAN 构建留出替换点。

- `MT_DPDK_LIB_NAME "MT_DPDK"`：所有 DPDK 内存池/分配在 rte_malloc 中的命名空间标签。
- 堆版：`mt_malloc / mt_zmalloc / mt_free` —— 直连 libc，仅用于控制面。
- hugepage 版：`mt_rte_malloc_socket / mt_rte_zmalloc_socket / mt_rte_free` —— 包 `rte_malloc_socket`，强制 `RTE_CACHE_LINE_SIZE` 对齐 + 显式 socket_id（呼应"NUMA mismatch 约使 DMA 延迟翻倍"的铁律）。
- `MTL_HAS_ASAN`：rte 版声明改指 `mt_util.c` 里的可追踪实现（`mt_asan_check`、记账版 `mt_rte_free`），用于发现释放后使用/泄漏。

**两世界规则的机械执行点**：数据面 tasklet 只用 `mt_rte_*`，控制面才允许 `mt_malloc`。

---

## `mt_platform.h`（159 行）

**一句话**：Linux/Windows 差异的集中垫片——头文件包含、时钟 ID、锁文件路径，以及带共享内存语义的 pthread 包装。

- 平台分叉：Windows 走 `win_posix.h`（`lib/windows/`）；Linux 拉入 numa/poll/ioctl/shm 等；`MTL_HAS_AVX512` 时带 `immintrin.h`。
- 时钟选择：
  - `MT_CLOCK_MONOTONIC_ID` = `CLOCK_MONOTONIC_RAW`（TSC 换算、PTP servo 用）；
  - `MT_THREAD_TIMEDWAIT_CLOCK_ID`：Linux 用 `CLOCK_MONOTONIC`（Windows 退 `CLOCK_REALTIME`），配合 `mt_pthread_cond_wait_init` 把 cond 的计时钟设成它——`pthread_cond_timedwait` 不再受改系统时间影响（PTP step 校时时调度 sleep 不漂移）。
- `MT_FLOCK_PATH`：lcore 仲裁的文件锁（Linux `/tmp/kahawai_lcore.lock`，Windows `c:/temp/...`），`mt_sch.c` 用。
- `mt_pthread_mutex_init`：默认给 mutex 加 `PTHREAD_PROCESS_SHARED`（Linux）——共享内存里放锁，跨进程可用（`mt_instance`/manager 体系的前提）。
- 其余 `mt_pthread_*`：薄封装 + `mt_pthread_cond_timedwait_ns`（纳秒超时）/ `timespec_add_ns`。
- `mt_socket_match`：CPU/设备 NUMA socket 匹配判定（Windows 恒 true）。

---

## `mt_header.h`（67 行）

**一句话**：最小的内部公共词汇表——句柄类型标签、裸包头、统计原语、pcap 状态。

- `enum mt_handle_type`：每个公开 handle 的 `type` 字段取值（`MT_HANDLE_MAIN/TX_VIDEO/RX_VIDEO/…/ST22_HANDLE_PIPELINE_TX/…/MT_HANDLE_UDMA/UDP`），用于公开 API 入口的类型 sanity 检查（`impl->type` 比对）与 `mt_handle_guard` 的类型拒绝。
- `struct mt_udp_hdr`：eth(14)+ipv4(20)+udp(8)=42 字节 packed 模板；TX 会话预组包头和 RTCP/ARP 组帧都基于它。
- `struct mt_stat_u64`：min/max/sum/cnt 统计原语（update helper 在 `mt_util.h`）。
- `struct mt_rx_pcap`：RX pcap dump 状态（文件、已 dump/dropped/目标包数、`usdt_dump` 标记）——配合 `mt_pcap.c` 与 USDT 探针使用。

---

## `mt_quirk.h`（24 行）

**一句话**：单一用途的编译器规避层——clang release 下 `rte_ring_sc_dequeue_bulk` 对变长 `n` 的退化代码序会产生错误结果，这里把 `n==4 / n==1` 拆成常量特化分支。

唯一一处"上游 DPDK/编译器 bug"的显式 workaround 登记处；GCC 不受影响、直穿。

---

## `mt_simd.h`（44 行）

**一句话**：按函数节切换 SIMD 指令集目标的宏设施——让一个 `.c` 文件同时携带 SSE4.2/AVX2/AVX512/VBMI 多份实现。

- `MT_TARGET_CODE_START(Target) / MT_TARGET_CODE_STOP`：clang 用 `#pragma clang attribute push(__attribute__((target(...))))`，GCC 用 `push_options + optimize("target")`，MSVC 空（intrinsics 本身不需要）。
- 预设档位：`MT_TARGET_CODE_START_SSE4_2`，及编译宏 `MTL_HAS_AVX2 / MTL_HAS_AVX512 / MTL_HAS_AVX512_VBMI2` 开启后的 AVX2 / AVX512(f,cd,vl,dq,bw) / VBMI2 组合。
- 消费方：`st2110/st_avx2.c`、`st_avx512.c`、`st_avx512_vbmi.c`（SIMD 色彩转换）和 `mt_util.c` 内个别路径；运行时由 `mtl_get_simd_level`（`mt_main.c`）选择调用哪一份。

---

## `mt_handle_guard.h`（159 行）

**一句话**：公开会话句柄的 use-after-free 防护门——引用计数 + 销毁标志的 C11 原子实现，**对 ARM 弱内存序（RK3588）关键**。

问题：reader 线程判定句柄存活后、解引用前的窗口里，另一线程跑完 `*_free()` 并释放内存。

方案（头文件注释即设计文档）：

- 每个公开 wrapper 结构带 `_Atomic uint32_t lc_destroying / lc_refcnt`；8 种可阻塞的 pipeline ctx 另带 `wake_on_destroy` 回调。
- 公开 API 入口 `MT_HANDLE_GUARD(h, MT_HANDLE_X, -EIO)`：类型不符或已 destroying 直接返回；否则 refcnt+1。**所有出口路径（含 goto）必须 `MT_HANDLE_RELEASE`**，漏一次 = `*_free` 在 drain 里永久自旋。
- `*_free`：`mt_handle_begin_destroy`（CAS 0→1，胜者唯一，败者 -EBUSY）→ `wake_on_destroy` 广播唤醒阻塞在 get/put_frame 的持有者 → `mt_handle_drain` 自旋至 refcnt==0 → 真正拆解 + `mt_rte_free`。
- 内存序：acquire（fetch_add SEQ_CST + load destroying SEQ_CST）与 destroy（CAS SEQ_CST + load refcnt SEQ_CST）构成 **Dekker store-buffer 对**，四个操作全部 SEQ_CST 才排除弱序 CPU（ARM）上的重排窗口；注释明确警告不得把 drain 的 load 降为 ACQUIRE。
- 纯 C11 `<stdatomic.h>`，无编译器扩展——与 `lib/` 的 C + stdatomic 约定一致。

---

## `meson.build`（35 行）

**一句话**：lib 核心的编译单元清单与平台分叉。

- `sources` 固定 18 个 `.c`：`mt_main, mt_sch, mt_cni, mt_ptp, mt_arp, mt_dhcp, mt_mcast, mt_util, mt_dma, mt_admin, mt_config, mt_socket, mt_stat, mt_rtcp, mt_flow, mt_instance, mt_log, mt_pcap`。
- Windows：`enable_tap` 选项为 true 时加 `mt_tap.c`；恒加 `../windows/win_posix.c`。
- `subdir('datapath') / subdir('dev') / subdir('st2110')` 递归三个子目录——即"根目录只管非媒体核心，数据面与媒体会话各自成组"的边界声明。

---

## 小结：本组文件的关系

```text
mt_header.h ── 词汇表（handle type / udp 模板 / stat 原语）── 全部模块
mt_mem.h ───── 分配入口（控制面 vs 数据面 + ASAN 开关）
mt_platform.h ─ OS 垫片（pthread/时钟/锁路径）
mt_quirk.h ─── 编译器 workaround
mt_simd.h ──── SIMD 分节（被 st2110 消费）
mt_handle_guard.h ─ 句柄生命周期门（被 include/ 公开 API 与 st2110 消费）
meson.build ── 装配上述一切进 libmtl
```
