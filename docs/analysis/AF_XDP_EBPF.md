# AF_XDP 与 eBPF 在 MTL 中的使用，及 RK3588 应用集成方案

分析对象：本仓库（feat/rk3588-apps 分支，版本 26.09.0.DEV）。配套阅读：`docs/analysis/ARCHITECTURE.md`（数据路径后端）、`docs/dev/01-MTL编译体系分析.md`（构建依赖 RB2/RB9）、`docs/dev/02-x86到RK3588交叉编译分析.md`（§4.2/§4.5）。

## 0. 一句话结论

MTL 把 AF_XDP 作为除 DPDK PMD 之外的"生产级"数据面后端：用户态由 `lib/src/dev/mt_af_xdp.c` 直接操作 `xsk_socket`/UMEM（复用 DPDK mbuf 内存做零拷贝），内核态由 MtlManager 加载 `manager/mtl.xdp.c` 这个 eBPF 程序做 UDP 端口过滤并把包重定向到 AF_XDP socket；两者通过 MtlManager 的实例 IPC（xsks_map fd + 端口过滤表）协作。两个 rk3588 应用已经配置了 `native_af_xdp:` 端口，但集成是否真正生效取决于构建期编入了 `MTL_HAS_XDP_BACKEND`、运行期 MtlManager 在板上跑起来且内核具备 `CONFIG_BPF_SYSCALL/CONFIG_XDP_SOCKETS/CONFIG_BPF_JIT`。

> **板上实测校正（2026-10-24，`docs/env/RK3588_BOARD_ENV.md`）**：`CONFIG_XDP_SOCKETS`/`CONFIG_BPF_SYSCALL` 均 `=y`，AF_XDP socket 全流程（含 `XDP_UMEM_UNALIGNED_CHUNK_FLAG`）实测可用；但 **`CONFIG_BPF_JIT` 未开、无 BTF、且内核根本没开 `CONFIG_HUGETLBFS`（无任何 hugepage）**，同时板载网卡（Motorcomm **yt6801**）不支持 native XDP、板上也没有 libxdp/xdp-tools/clang。所以本文档中"网卡 = r8168/RTL8125""内核版本较老待确认"等前提已被实测替换，落地顺序见 §6 的修正标注。

## 1. 仓库中的三处 eBPF/AF_XDP 相关代码

| 位置 | 角色 | 依赖 |
|---|---|---|
| `lib/src/dev/mt_af_xdp.c`（约 1040 行） | 数据面：创建 UMEM/xsk，RX 收包、TX 发包（native AF_XDP 后端） | libxdp + libbpf（构建期探测，见 §2） |
| `manager/mtl.xdp.c` + `manager/mtl_interface.hpp` | 控制面：eBPF 过滤程序源码 + MtlManager 加载/挂载/更新 map | libxdp、libbpf、clang + llvm-strip（构建 `mtl.xdp.o`） |
| `tools/ebpf/`（et、lcore_monitor、udp_monitor） | 诊断工具：fentry/tracepoint 类 eBPF 程序，观测 UDP 发送耗时、调度器被抢占/中断打断情况 | libbpf、bpftool，独立 Makefile，与库运行无关 |

另有两个外围接触点：`script/build_ebpf_xdp.sh`（按 `versions.env` 钉的 xdp-tools 1.6.3 / libbpf 构建安装，并检查内核 `CONFIG_BPF_SYSCALL`、`CONFIG_XDP_SOCKETS`、`CONFIG_BPF_JIT`）；`patches/dpdk/22.07/0002-net-af_xdp-*.patch` 等是 DPDK 侧 `rte_eth_af_xdp` 虚拟 PMD（`MTL_PMD_DPDK_AF_XDP`，实验性）的补丁，与 native 路线无关。

## 2. 构建期：AF_XDP 后端如何被编入

`lib/src/meson.build` 对 `libxdp` 和 `libbpf` 做 `required: false` 探测，**两者都找到**才定义 `MTL_HAS_XDP_BACKEND` 并把 `mt_af_xdp.c` 编入库；缺任一则静默退化为"无 AF_XDP 后端"，`mtl_init` 收到 `MTL_PMD_NATIVE_AF_XDP` 时会在 `mt_dev_xdp_init` 报错退出（"AF_XDP backend must run with MTL Manager!"之前先是没有后端可走）。所以 RK3588 构建脚本必须：

1. 先跑 `script/build_ebpf_xdp.sh`（或用 sysroot 里的 libxdp/libbpf 交叉版本）；
2. 构建后断言 `grep MTL_HAS_XDP_BACKEND` 在 meson 日志/config.h 中出现（对应文档 01 的 RB2/RB18）。

`mt_af_xdp.c` 顶部硬性要求 libxdp 带 `XDP_UMEM_UNALIGNED_CHUNK_FLAG`（不支持直接 `#error`），即版本不能太老。

## 3. 数据面：mt_af_xdp.c 的工作方式

**初始化（`mt_dev_xdp_init`）**

- 仅当端口 `pmd == MTL_PMD_NATIVE_AF_XDP`（端口名 `native_af_xdp:<if>`）时走这条路径。
- **强制要求已连接 MtlManager**：`mt_is_manager_connected(impl)` 为假直接返回 `-EIO`，即 native AF_XDP 不能裸 root 跑——队列分配和 XDP 程序都由 manager 掌管。
- 队列数 `queues_cnt = max(nb_tx_q, nb_rx_q)`，每个队列通过 `mt_instance_get_queue(impl, ifindex)` 向 manager 申请一个空闲硬件/内核队列号。

**每个队列的建立（`xdp_socket_init`）**

1. `xdp_umem_init`：把该队列的 DPDK mbuf mempool 的**整块内存直接注册为 UMEM**（`xsk_umem__create`，`XDP_UMEM_UNALIGNED_CHUNK_FLAG` 非对齐模式，headroom 覆盖 mbuf header），因此 AF_XDP 收到的帧就是 mbuf，零拷贝、无 memcpy；
2. `xdp_rx_prod_init`：预填充 FILL ring（把 mbuf 地址换算成 umem 偏移塞入）；
3. `xsk_socket__create`：先试 `XDP_ZEROCOPY` 强制绑定（`XDP_FLAGS_UPDATE_IF_NOEXIST`），失败（如网卡驱动不支持）回退 copy 模式；若 `has_ctrl`（连着 manager）则 `XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD` 跳过 libxdp 内置 xsk 程序加载，改由 manager 的 dispatcher 统一管理，随后用 `xdp_socket_update_xskmap` → `mt_instance_request_xsks_map_fd()` 从 manager 拿 xsks_map fd，`xsk_socket__update_xskmap` 挂上去；
4. 若无 manager 且非 root 会报 "please run with mtl manager or root user"。

**收发**

- RX：`mt_rx_xdp_burst` 从 `rx_cons` 收包，校验后按 umem 偏移还原成 `rte_mbuf*` 交付上层，再从 FILL ring 补新的空 mbuf；支持的组播走独立 socket fd（`mcast_fd`）。
- TX：`mt_tx_xdp_burst` 把 mbuf 地址填入 `tx_prod`，`xdp_tx_poll_done`/`xdp_tx_check_free` 轮询 COMPLETION ring 回收 mbuf；`xdp_tx_wakeup` 处理 need-wakeup。
- **限速**：`xdp_queue_tx_max_rate` 通过 sysfs（`/sys/class/net/<if>/queues/tx-<q>/tx_maxrate`，注意属性名是 `tx_maxrate`）设置队列级限速，`drv_info.rl_type = MT_RL_TYPE_XDP_QUEUE_SYSFS`——这是 x86 之外（含 RK3588）唯一可用的"硬件" pacing 途径。
  - 但**探测是驱动白名单制的**：`mt_af_xdp.c:792` 只在 `strncmp(xdp->drv, "ice", …) == 0` 时才调 `xdp_parse_pacing_ice()`，而后者写死探测 `tx-1` 队列（`mt_af_xdp.c:284`）。
  - 因此除 ice 外的任何驱动（含 RK3588 板载网卡）都**不会**启用 RL，`rl_type` 保持默认，直接回落 TSC 软件 pacing。
  - RK3588 实测量见 `docs/env/RK3588_BOARD_ENV.md` §4.4：板载网卡只有 `tx-0`（无 `tx-1`），且手工写 `tx_maxrate` 返回 `EOPNOTSUPP`。
- 会话创建/销毁时（`mt_rx_xdp_get`/put、tx flow），若有控制面则调 `xdp_socket_update_dp` → `mt_instance_update_udp_dp_filter(impl, ifindex, dst_port, add/del)`，让 manager 更新 eBPF 过滤表（见 §4）。

## 4. 控制面：MtlManager + mtl.xdp.c

**eBPF 程序 `mtl.xdp.c`**（构建期由 meson `custom_target` 用 clang 编成 `mtl.xdp.o`，安装到 `<prefix>/lib/bpf/`）：

- 一张 `BPF_MAP_TYPE_HASH` map `udp4_dp_filter`（key: 16bit UDP 目的端口，max 256 条）；
- XDP 程序 `mtl_dp_filter`（`SEC("xdp")`，priority 19）：解析 eth→ip→udp，目的端口命中过滤表则 `XDP_PASS`（交给后续程序/内核），否则——注意语义是**反直觉的方向**：命中表的端口 `return XDP_PASS`，未命中 `return XDP_PASS`，真正"DROP"分支注释为"go to next program: xsk_def_prog"。实际效果由 libxdp 的 **xdp-dispatcher 链**决定：`mtl_dp_filter` 挂在 dispatcher 里，排在 libxdp 内置的 `xsk_def_prog` 之前；命中过滤端口的包被前一个程序 `XDP_DROP`（不出协议栈，因为该包将由 AF_XDP socket 收），其余包 PASS 给 `xsk_def_prog` 重定向到 xsks_map 对应的 socket。也就是说这张表是"哪些 UDP 端口**不**走 XSK 重定向"的旁路表（manager/README.md 说明用 `xdp-loader status` 可查看链上程序）。
- `manager/mtl_interface.hpp`：每个接口 `load_xdp()` → `xdp_program__find_file("mtl.xdp.o")` → `xdp_program__attach`（先 `XDP_MODE_NATIVE`，失败退 `XDP_MODE_SKB`）→ `xsk_setup_xdp_prog` 建 xsks_map 并保存 `udp4_dp_filter_fd`。`update_udp_dp_filter()` 用 `bpf_map_update_elem` 增删端口表项。

**IPC**：`lib/src/mt_instance.c` 在实例注册时把每个 AF_XDP 端口的 `ifindex` 报给 manager（`mtl_mproto.h` 协议）；运行中通过 `MTL_MSG_TYPE_ADD_UDP_DP_FILTER` / `DEL_UDP_DP_FILTER` 消息和 `MSG_GET_XSKS_MAP` 类请求完成 §3 的 fd 分发与端口表更新。所有 eBPF map 操作都在 manager（root）进程，应用进程只需 `CAP_NET_RAW` 建 xsk——这正是 MTL "非 root 运行"设计的落点。

**tools/ebpf/ 诊断工具**（可选，x86 上使用，ARM 同理可交叉/本机编）：

| 工具 | 挂载点 | 用途 |
|---|---|---|
| `et`（fentry.bpf.c） | `fentry/fexit udp_send_skb` | 测 kernel socket 路径每包发送耗时 |
| `lcore_monitor` | tracepoint `sched_switch` + `irq_handler_entry` | 检查调度 lcore 被谁打断（调优绑核） |
| `udp_monitor` | raw socket + `SO_ATTACH_BPF` | 抓 UDP 头统计 |

## 5. RK3588 应用的现状

两个应用**均已按 native AF_XDP 配置**，不是"将来要用"，而是"已写好、待环境满足"：

- `app/rk3588_end2end/e2e_config.c`：`tx_port = "native_af_xdp:enP3p49s0"`、`rx_port = "native_af_xdp:eth0"`；`e2e_app.c: mtl_setup_single()` 固定 `pmd[MTL_PORT_P] = MTL_PMD_NATIVE_AF_XDP`，`net_proto = MTL_PROTO_STATIC`，且注释明确了单队列约束：TX-only 也要 `rx_queues_cnt=1` 才能在 `mt_dev.c` 选 1q 路径。
  - **板上实测校正**：网卡**不是 r8168/RTL8125**，两个口都是 **Motorcomm yt6801**（builtin，`drivers/ztl/yt6801`）。`enP3p49s0` 就是 **eth1**，当前**未插网线（NO-CARRIER）**；唯一有链路的是 **eth0 = `enP2p33s0`**，且只协商到 **100 Mb/s**——单路 1080p 需 ~1.2 Gbps，物理链路先决。队列实测 **RX 4 / TX 1**，"单队列"只在 TX 侧成立，代码里 `rx_queues_cnt=1` 的写法仍安全但理由已变。
- `app/rk3588_mp4_dist/mp4_dist_config.c`：`mp4_dist_pmd_from_port()` 按端口名前缀解析（`native_af_xdp:` → NATIVE_AF_XDP，`kernel:` → KERNEL_SOCKET，`dpdk_af_xdp:` → DPDK_AF_XDP），`mp4_dist_mtl_init()` 额外处理了两个单队列关键点：
  - ST22 视频 + ST30 音频同端口时必须 `MTL_FLAG_SHARED_RX_QUEUE`/`MTL_FLAG_SHARED_TX_QUEUE`（rsq/tsq demux），否则第二个会话覆盖 xdp queue 的 `rx_entry`，音频 tasklet 在 `mt_rxq_burst` SIGSEGV；
  - PTP 走 `ptp_get_time_fn` 外部 CLOCK_TAI（linuxptp），因为板载网卡无 HW timesync（**实测**：`ethtool -T eth0` 只有 `software-receive`/`software-system-clock`，PTP Hardware Clock = none，且板上无 `/dev/ptp*`、未装 `ptp4l`/`phc2sys` → 该方案当前不可用，需装 linuxptp 或改用系统时钟）。

端到端链路（V4L2→MPP→MTL TX→MTL RX→MPP→DRM）与 mp4_dist 都只经 `mtl_init`/`st22p_*`/`st30p_*` 标准 API，**不直接触碰 AF_XDP**——后端选择完全由 `mtl_init_params.pmd/port` 决定，这是集成的关键优点。

## 6. RK3588 上启用 AF_XDP 的集成清单

前提差异：RK3588 无 E810，板载网卡是 **Motorcomm yt6801（PCIe，builtin 驱动，实测 RX4/TX1）**，两个口都不支持 native XDP；内核是 **Rockchip BSP 6.1.118（实测，见 `docs/env/RK3588_BOARD_ENV.md`）**，AF_XDP socket 层能力已实测通过。按顺序：

**构建期（对应 docs/dev/01 §8、02 §4.2）**

1. 确认板内核：`zcat /proc/config.gz | grep -E "XDP_SOCKETS|BPF_SYSCALL|BPF_JIT|HUGETLBFS"`。
   - **实测通过（2026-10-24）**：`CONFIG_XDP_SOCKETS=y`、`CONFIG_BPF_SYSCALL=y` ✅；`XDP_UMEM_UNALIGNED_CHUNK_FLAG` 可用（raw-syscall 探针在 eth0/eth1 上 7 步全 `ret=0`，含 `bind`）✅。
   - **实测不通过**：**`CONFIG_BPF_JIT` 未开**（eBPF 只能解释器执行）、**无 BTF**（`/sys/kernel/btf/vmlinux` 缺失）、**`# CONFIG_KPROBES is not set`**（`tools/ebpf/` 的 `et`/`lcore_monitor` 不可用）、**`# CONFIG_HUGETLBFS is not set`**（无任何 hugepage）❌。
   - `script/build_ebpf_xdp.sh --check` 会检查那三项 CONFIG，本板会在 `BPF_JIT` 上失败。**必须先重编内核打开 `HUGETLBFS`（+`BPF_JIT`，建议 `DEBUG_INFO_BTF`）**——这是挡住 MTL 全部后端（不止 AF_XDP）的第一阻塞项，`sysctl -w vm.nr_hugepages` 在不支持的内核上无效。
2. `script/build_ebpf_xdp.sh` 构建 xdp-tools/libbpf（交叉则按文档 02 §4.2 的 make 变量方式），再 `./build.sh`，并断言 `MTL_HAS_XDP_BACKEND` 已定义。**实测**：板上既无 `libxdp`/`xsk.h`/`xdp-loader`，也无 `libbpf-dev`（只有运行库 `libbpf0 1:0.5.0`），apt 无 `libxdp-dev`/`xdp-tools` 候选，且**无 clang/llvm-strip/libtool** → 这一步只能交叉编译后部署，板上原地编不出来。
3. MtlManager 必须带 XDP 构建（找到 libxdp/libbpf 时 manager 强制要求 clang + llvm-strip 编 `mtl.xdp.o`）；板上 `/usr/local/lib/bpf/mtl.xdp.o`（或对应 prefix）必须存在——`xdp_program__find_file` 按该路径找。**实测**：板上 `/usr/local` 无任何 MTL 产物（无 `libmtl*`、无 `MtlManager`），DPDK 也未安装。

**运行期**

4. `sudo sysctl -w vm.nr_hugepages=2048`（AF_XDP 后端同样吃 hugepage，因为 mempool 是 hugepage 内存直接注册 UMEM）。**实测**：本板内核无 `HUGETLBFS`，此命令无效——`/proc/meminfo` 无任何 `Huge*` 行、`/sys/kernel/mm/hugepages/` 不存在、`/dev/hugepages` 不存在；`CONFIG_CMA=y`（16 MiB）**不能**当 hugetlb 用。见 §6 构建期第 1 条，须先重编内核。
5. `sudo MtlManager` 先启动（应用必须连上 manager，否则 `mt_dev_xdp_init` 直接失败；非 root 应用还需 `CAP_NET_RAW`）。
6. 端口名保持 `native_af_xdp:<if>`；单队列网卡两个应用的 `tx_queues_cnt/rx_queues_cnt=1` 约定与 mp4_dist 的 SHARED_QUEUE flag 已在代码里，不要改掉。**实测注意**：板上 `enP3p49s0` = eth1（未插线），活动口是 eth0（`enP2p33s0`，仅 100 Mb/s），选口前先确认链路。

**特有风险与对策**

| 风险 | 说明 | 对策 |
|---|---|---|
| 板载网卡不支持 XDP native / zero-copy | **已实测证实**（非"可能"）：yt6801 上 `ip link set ... xdpdrv` 返回 `Underlying driver does not support XDP in native mode.`，`xdpgeneric` 成功 | 代码已自动退 `XDP_MODE_SKB` 挂载与 copy 模式；再不行退 `kernel:` 后端（mp4_dist 已支持按端口名切换）。要性能只能换网卡或给驱动补 `ndo_bpf` |
| 队列限速 pacing 不可用 | **两层原因**：① MTL 的 RL 探测只对驱动名 `ice` 执行（`mt_af_xdp.c:792`），rk3588 板载网卡压根不会被探测；② 即便手工写 `tx-0/tx_maxrate` 也返回 `EOPNOTSUPP`（实测），且探测函数写死的 `tx-1` 在本板根本不存在（只有 `tx-0`） | 用默认 TSC 软件 pacing（当前 rk3588 应用的既定选择，见 req/02 §2.2 R6） |
| `BPF_JIT` 未开 + 无 BTF | **实测**：`# CONFIG_BPF_JIT is not set`（eBPF 走解释器，µs pacing 性能致命）、无 `/sys/kernel/btf/vmlinux`、`# CONFIG_KPROBES is not set` | 重编内核打开 `BPF_JIT`/`DEBUG_INFO_BTF`/`KPROBES`；在此之前 `tools/ebpf/`（`et`/`lcore_monitor`）全部不可用 |
| `mtl.xdp.o` 与板上内核不匹配 | clang 编译的 BPF 依赖内核头兼容性。原对策（`bpftool btf dump`）**在本板不可用**——无 BTF 也无 bpftool | `mtl.xdp.c` 是 plain XDP + hash map，理论上不依赖 BTF，但板上无 libxdp **尚未实测加载**；装载失败看 `xdp-loader status`（需先部署 xdp-tools） |
| manager 未跑/版本不匹配 | `mt_dev_xdp_init` 报 must run with MTL Manager | 部署脚本把 MtlManager 作为 systemd 服务先于应用启动 |
| 无 hugepage（新发现，优先级高于以上所有） | 内核 `# CONFIG_HUGETLBFS is not set`，MTL 任何后端都起不来 | 重编内核 + 建 hugetlbfs 挂载点，见 §6 构建期第 1 条与 `docs/env/RK3588_BOARD_ENV.md` §2.1 |

**最小验证路径**

> 前置：内核已开 `HUGETLBFS`（+`BPF_JIT`）、libxdp/xdp-tools 已部署、MTL 与 MtlManager 已装到板上。**这三项当前本板全部不满足**，下列命令要在补齐后才能跑。

```bash
# 板上
sudo sysctl -w vm.nr_hugepages=2048     # 本板内核无 HUGETLBFS，现在执行无效
sudo MtlManager &                 # 等 manager 就绪
xdp-loader status                 # 确认 mtl_dp_filter 挂上（本板暂无 xdp-loader，需部署）
./e2e_app rx  --port native_af_xdp:eth0 &       # eth0 = enP2p33s0，当前唯一有链路的口
./e2e_app tx  --port native_af_xdp:enP3p49s0    # = eth1，实测未插线(NO-CARRIER)！
# 回归确认：lib/configure 日志有 MTL_HAS_XDP_BACKEND；manager 日志有 udp4_dp_filter 更新
```

若任一步失败，回退序：native AF_XDP → `kernel:` socket 后端（功能可用、性能低）→ 均不可用时再评估 DPDK af_xdp 虚拟 PMD（`MTL_PMD_DPDK_AF_XDP`，实验性）。**注意**：本板 native 模式已被实测排除，AF_XDP 一上来就是 generic XDP + copy UMEM 形态，性能基准要按这个前提设定；而 `kernel:` 后端是唯一"现在就能跑"的路径（但仍被 hugepage 挡住）。
