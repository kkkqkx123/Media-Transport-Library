# 剩余任务清单（RK3588 分支 / DVS 项目）

> 整理日期：2026-11-03
> 输入：
> - `docs/archive/`：`RK3588_VERIFICATION_RESULTS.md`（2026-11-02 实测）、`RK3588_CROSS_COMPILE_PROGRESS.md`、`RK3588_5.10_DEBIAN_ACCOUNTS.md`
> - `docs/dev/RK3588_VERIFICATION_PLAN.md`
> - `docs/req/01~06`
> - `docs/plan/`：`RK3588_SHARED_CODE_EXTRACTION.md`、`RK3588_LIVE_INGEST_IMPROVEMENT_PLAN.md`、`DVS_IPC_ASYNC_PLAN.md`、`DVS_FRONTEND_INTEGRATION_PLAN.md`
> - `patches/rk3588-5.10/`：`0001`(AF_XDP/BPF 配置)、`0002`(YT6801 驱动 + ttyS3 上报 + rootfs)、`base/kernel/arch`、`superseded/`
>
> 状态图例：⬜ 未开始　🟨 部分完成　✅ 已完成　⛔ 阻塞

---

## 0. 总览与结论

- **当前进度**：userspace 交叉编译（DPDK+MTL+libxdp+rk3588 应用）已全部打通 ✅；库加载/L1 组件测试（6 项）/MTL 传输初始化/L2 ✅；`file_playout_tx` 内核后端 TX ✅。
- **最大阻塞（内核/驱动）**：板端 `yt6801` 网卡驱动未实现 `ndo_bpf`/`ndo_xdp_xmit`，**AF_XDP native 模式不可用**。当前只能走 `kernel:` 后端（见验证归档 §11）。这是唯一需要内核级代码改动才能解锁的能力。
- **验证缺口**：L3 完整音视频流、L4 解码/显示、L5 双机 PTP、L6 长稳均未跑通，主因是缺片源 `InTheGrey.mp4`、物理 HDMI 显示器、`linuxptp`。
- **应用/平台**：end2end 改进、共享库 C 档、IPC 异步化、dvs_media/dvs_agent/platform（S0–S5）全部 ⬜。

---

## 1. 内核与驱动修改（KERNEL）

| ID | 任务 | 状态 | 依赖 | 退出标准 |
|----|------|------|------|----------|
| **K1** | **YT6801 驱动实现 XDP 支持**（`ndo_bpf` / `ndo_xdp_xmit` / `ndo_xdp_flush`），解锁 MTL `native_af_xdp` | ⬜ | `0002` 驱动已 bringup | `live_ingest tx` 经 `native_af_xdp:enP2p33s0` 不再报 `Underlying driver does not support XDP`；MtlManager native attach 成功。需评估 YT6801 数据面改 XDP 回归成本 |
| **K1-alt** | 替代方案：换用支持 XDP 的 NIC/驱动（验证归档 §11.3 指出 USB 网卡通用 XDP 已可用），避免驱动改动 | ⬜ | 硬件到货 | 选定 NIC 在 5.10 下 `ethtool -i` 含 XDP 支持，AF_XDP 跑通 |
| **K2** | 基于 `patches/rk3588-5.10/` 构建并固化可部署内核镜像（`base/kernel/arch` + `0001` + `0002` + 可选 K1），记录独立的内核交叉编译/打包步骤（当前仅 userspace 有 `RK3588_CROSS_COMPILE_PROGRESS.md`） | 🟨 | 0001/0002 已存在 | `ztl_defconfig` 编译出可启动内核；板端 `CONFIG_XDP_SOCKETS`/`BPF_SYSCALL`/`DEBUG_INFO_BTF` 生效（可由 `/proc/config.gz` 或 `/boot/config` 校验） |
| **K3** | 确认/补齐 **DMA32 堆**：RGA 报 `unsupported memory larger than 4G`（改进方案 §2.2 P2）。内核需保证 `system-uncached-dma32` 堆可用或保留低于 4G 的 DMA 可寻址区 | ⬜ | 硬件确认 | end2end `dma32_allocator` 路径可走内核 dma-heap 而非用户态绕过；RGA 大于 4G 地址告警消失 |
| **K4** | 内核 PTP/PHC 配置确认：`CONFIG_PTP_1588_CLOCK`、网卡 PHC 可用；配合 `linuxptp` 双机同步（验证归档 L5 未跑） | ⬜ | K2 镜像 | `phc_ctl`/`ptp4l` 在板端跑通，offset ≤ 2ms |
| **K5** | MtlManager 的 `xdp-dispatcher.o` 部署与驱动加载联动（已装 `/lib/bpf/`，K1 完成后需驱动侧加载） | ⬜ | K1 | AF_XDP 多队列/共享模式可用 |

> 决策点：**K1 vs K1-alt** 决定后续所有 AF_XDP 路径验证的可行性。建议先评估 YT6801 数据面 XDP 化的工作量（可能很大），再决定是否走替换 NIC 路线。在 K1 完成前，验证一律走 `kernel:` 后端。

---

## 2. 验证任务（VERIFY）

> 分层对应 `docs/dev/RK3588_VERIFICATION_PLAN.md` 的 L0–L6。

| ID | 任务 | 状态 | 依赖 | 退出标准 |
|----|------|------|------|----------|
| **V1** | L1：`test_codec`/`test_io`/`test_pipeline` 在**接入 HDMI 显示器**后跑通 | ⬜ | 物理显示器 | 三测试 `0 failed`；`test_pipeline` RX 解码路径 ready（当前仅 TX ready） |
| **V2** | L3：`live_ingest` 完整音视频端到端（需 `InTheGrey.mp4` + MtlManager + 后端） | ⬜ | 片源 + K1 或 kernel 后端 | 板端 RX 收到帧、解码、DRM 出画、ALSA 出声 |
| **V3** | L3：`file_playout` 单板/双机全链路（TX 发流 + RX 出画 + ALSA 播放） | 🟨 | 片源 + 显示器 | `file_playout_rx` 出现 MPP 解码成功、DRM pageflip、`av_sync clock=ptp`（或 monotonic 回退） |
| **V4** | L5：双机 PTP（`linuxptp` 安装 + 偏差验证） | ⬜ | K4 + 第二台机器 | `ptp4l` master/slave 同步，`offset ≤ 2ms`，断网可恢复 |
| **V5** | L4：DRM 渲染到**物理 HDMI 输出**（含 `rx_compositor` 多 plane 合成） | ⬜ | 显示器 | 显示器出现画面，多窗口叠加正确 |
| **V6** | ALSA **HDMI 输出**播放验证（输入已 ✅，见验证归档 §12） | ⬜ | 显示器 | `hw:?,0` HDMI 输出出声，音量/静音生效 |
| **V7** | `rx_minimal`/`file_playout_rx` 内核后端**回环收帧 0 frames** 根因排查（验证归档 §9.2 同机回环未收帧） | ⬜ | 片源或对端设备 + `tcpdump` | 明确是驱动回环限制还是配置问题；给出可复现的收帧命令 |
| **V8** | AF_XDP 端到端（依赖 K1/K1-alt） | ⬜ | K1 | `live_ingest`/`file_playout` 走 `native_af_xdp` 收发正常 |
| **V9** | L6：24h 稳定性长稳（`file_playout_rx 300` 监控 CPU/丢包/解码/温度） | ⬜ | V3/V5 | 24h 无崩溃、无持续丢帧、无降频重启 |
| **V10** | PTP 资格阈值实测：NFR-04 同帧偏差 ≤1 帧、`apply_at` guard 取值（req 05 §6.5 / 04 §6） | ⬜ | V4 + S4 | 实测偏差回填阈值表 |

---

## 3. 构建 / 工具链 / 环境（BUILD）

| ID | 任务 | 状态 | 依赖 | 退出标准 |
|----|------|------|------|----------|
| **B1** | 提交应用源码 `#include <stdbool.h>` 补丁（约 40 个 `.c`，交叉编译必需，原生编译碰巧引入） | ⬜ | 无 | 改动 commit 入 `feat/rk3588-apps`；原生 + 交叉编译均通过 |
| **B2** | sysroot 环境再同步：devs 新增 libxdp/MTL 产物/FFmpeg 软链/`numaif.h` 补丁 rsync 回本地 Arch 机 | ⬜ | 无 | 本地可独立 `meson setup build && ninja` 应用 |
| **B3** | 板端部署 MtlManager + `xdp-dispatcher.o`（已构建，待启用/固化到镜像） | 🟨 | 无 | MtlManager 随镜像启动并监听；K1 后 AF_XDP 多实例可用 |
| **B4** | 板端环境固化进镜像：hugepages 512×2MB、MTL mempool ops `ring_mp_mc`、DPDK 插件仅留 `mempool_ring`（当前手动，验证归档 §1） | ⬜ | K2 镜像 | 镜像启动后无需手动修复即可 `mtl_init` 成功 |
| **B5** | sysroot 补齐 FFmpeg 未版本化软链（`libavformat.so` 等） | ✅/⬜ | B2 | `dependency('libavformat')` 解析成功 |
| **B6** | 板端安装 `linuxptp`（`ptp4l`/`phc2sys`） | ⬜ | 无 | `ptp4l --version` 可用 |

---

## 4. 应用与媒体引擎改造（APP）

### 4.1 共享库抽取（SHARED）

| ID | 任务 | 状态 | 依赖 | 退出标准 |
|----|------|------|------|----------|
| **A1** | C 档 `drm_renderer`/`mpp_decoder` 合并决策与执行：以 file_playout 接口为基准重设计，end2end 做适配层；或确认 dvs-media 直接消费 `rk3588_playout_rx/shared/` 副本（SHARED §2 / FRONTEND S0） | ⬜ | 无 | 决策记录落档；若合并则两 app 仍全绿 |
| **A2** | D 档模块（`drm_mode`/`display_layout`/`ptp_time`/`e2e_stamp` 等）迁入 common，供 dvs-media 复用（FRONTEND S0） | ⬜ | A1 | dvs-media 能 include common 头并链接静态库 |

### 4.2 end2end 采集/编码链路改进（IMP）

> 来源 `RK3588_LIVE_INGEST_IMPROVEMENT_PLAN.md`，当前 `test_pipeline` 仅 TX ready。

| ID | 任务（对应方案 §2 段落） | 状态 | 依赖 | 退出标准 |
|----|------|------|------|----------|
| **A3** | P1 SPS VUI 注入（修色偏，纯 C 模块 `codec/sps_vui.c` + 单测） | ⬜ | 无 | 接收端按 BT.709 full-range 解释，彩条比对无色偏 |
| **A4** | P1 V4L2 格式回读校验 + 内核时间戳优先 + 代际/认领位"恰好一次归还" | ⬜ | 无 | 无异步 dmabuf 竞态；无信号占位时 `-ENODATA` 周期重试而非带病启动 |
| **A5** | P1 编码异步化（Feeder/Collector 双线程 + 有界 FrameRing，消除忙等） | ⬜ | A4 | 编码耗时不再挤占采集/转换窗口 |
| **A6** | P1 HangGuard 看门狗（编码挂死销毁重建，连续超限禁用） | ⬜ | A5 | 注入编码挂死后自动恢复或粘滞禁用并告警 |
| **A7** | P2 RGA 池语义 + DMA32 堆 + 失败降级链（≥3 次切 mmap+CPU 拷贝） | ⬜ | A5 | RGA 失败不隐式复用被引用的 buffer |
| **A8** | P1 信号变化检测（`V4L2_EVENT_SOURCE_CHANGE`）+ P2 错误恢复（防风暴重建） | ⬜ | A4 | HDMI 热插拔触发管线重建而非崩溃 |
| **A9** | P2 统计汇聚 + 周期日志；P2 set_param RC 热重配（bitrate/gop/qp） | ⬜ | A5 | 统一 stats 结构；RC 重配不断流 |
| **A10** | 24h 长稳回归（每阶段按方案 §4） | ⬜ | A3–A9 | 丢帧/在途峰值/RGA·编码失败均为 0 或有界 |

---

## 5. DVS 控制面与节点软件（DVS）

> 来源 `DVS_IPC_ASYNC_PLAN.md` 与 `DVS_FRONTEND_INTEGRATION_PLAN.md`（S0–S5）。

### 5.1 IPC 异步化（IPC，S1 前提）

| ID | 任务 | 状态 | 依赖 | 退出标准 |
|----|------|------|------|----------|
| **D0-1** | `ipc/state_machine.{h,c}`：revision 幂等 / STALE_REVISION / INVALID_STATE / 六规则校验（方案 §2.1） | ⬜ | 无 | 单测覆盖幂等/STALE/INVALID 迁移 |
| **D0-2** | `ipc/ipc_server.c` 重写：长连接 + `messageId` 幂等 + `prepare` 异步(202+事件) + 事件广播 + write mutex | ⬜ | D0-1 | mock 客户端驱动 configure→prepare→commit→query_state 全链路 |
| **D0-3** | `dvs_agent/media_ipc`：持久连接+重连+requestId+poll 超时+后台事件线程+结构化错误码 | ⬜ | D0-2 | 对不存在 sock 返回 `MEDIA_ENGINE_UNREACHABLE` 不崩溃 |
| **D0-4** | `main.cpp` HTTP 层：`/api/v1/node/media/*` 上抛 + 新增 `/events` 拉缓存事件 | ⬜ | D0-3 | 引擎不可达时透传错误 envelope，不再空状态 |

### 5.2 分阶段实施（FRONTEND S0–S5）

| ID | 任务 | 状态 | 依赖 | 退出标准 |
|----|------|------|------|----------|
| **S0** | 公共库扩展（A1/A2 + 退出标准：两 app 编译/单测全绿、无行为 diff、dvs-media 可链接） | ⬜ | A1/A2 | 见 SHARED + FRONTEND S0 |
| **S1** | `dvs_media`：session_mgr / layout_executor(两段式) / audio_arbiter / capability / ipc_server / main；单节点 4 路热更新、帧边界切换、abort 回滚、能力 JSON | ⬜ | S0, D0-1..4 | 单节点 4×1080p60 窗口增删移动缩放调层序无需重启；mock 客户端驱动八条消息 |
| **S2** | `dvs_agent` + node-admin：六 scope 配置状态机(SQLite) / 注册心跳 / 维护操作 / 本机 HTTP / 前端 real-api.js | ⬜ | S1 | 浏览器本机页六 zone 真实反映状态；断网心跳超时→重连→上报 revision；升级校验失败版本不变 |
| **S3** | `platform` + platform 原型：数据模型 / REST(FLOW-01) / 布局像素计算 / 节点网关 WS / operation 最小闭环 | ⬜ | S2(可并行) | 两节点 2×1 墙走通建组→建墙→绑定→白名单→开窗→存场景，全真实出画 |
| **S4** | 同帧与跨格：prepare 屏障 + `apply_at` TAI→帧号 + 原子提交 + 回滚 + 跨格片段 + 底图下发 + 常驻订阅调优 | ⬜ | S1,S3 | 2×2 墙 1000 次切换同帧偏差 ≤1 帧；任一离线整墙 blocked；commit 失败自动回滚 |
| **S5** | 运维与规模化：退役/墓碑 / 批量升级 / 播放任务对接 / 统计落库 / 代码收尾 / 128 节点压测 + 4×4 墙 | ⬜ | S3 | 退役→墓碑→重连为既有设备；压测 NFR-01/08 达标；4×4 同帧达标 |

---

## 6. 文档回写（DOCS）

| ID | 任务 | 状态 | 依赖 | 退出标准 |
|----|------|------|------|----------|
| **DC1** | `docs/plan/README.md` 与 `AGENTS.md` Gotchas：更新"当前未抽取公共库，修改时注意两处同步"为"以 `app/rk3588_common/` 为准" | ⬜ | A1 | 描述与代码一致 |
| **DC2** | `RK3588_SHARED_CODE_EXTRACTION.md` §4 遗留待办：同步 `docs/app/README.md` 过时说明 | ⬜ | A1 | 引用路径更新 |
| **DC3** | `RK3588_VERIFICATION_RESULTS.md` 随 V1–V9 推进回写状态/结论 | ⬜ | V 系列 | 验证结果可追溯 |
| **DC4** | `DVS_FRONTEND_INTEGRATION_PLAN.md` 完成后回写 `docs/app/frontend_prototype_integration.md` 状态、req/05 阶段勾稽、AGENTS.md 新增 app 目录说明 | ⬜ | S0–S5 | 文档间一致 |
| **DC5** | 内核修改（K1–K4）结论回写 `docs/dev/` 相关分析，更新"AF_XDP 不可用根因"章节 | ⬜ | K1/K1-alt | 根因与处置一致 |

---

## 7. 建议的推进顺序（依赖链）

1. **先解阻塞**：K1/K1-alt 决策 → K2 镜像固化 → B4 环境固化。
2. **补齐验证**：B6+V4(PTP) → V1/V3/V5/V6(显示器) → V7(回环) → V9(长稳)。
3. **应用质量**：B1(stdbool 提交) → A3–A10(end2end 改进) → A1/A2(共享库)。
4. **DVS 主线**：D0(IPC 异步) → S0 → S1 → S2/S3(并行) → S4 → S5。
5. **文档**：DC1–DC5 随各阶段收尾回写。

> 并行建议：内核 K 系列与验证 V 系列可在有显示器/片源/第二台机后并行；DVS 控制面（S2/S3）在 S1 IPC 稳定后可并行起步。
