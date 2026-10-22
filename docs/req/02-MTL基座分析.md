# Media-Transport-Library 基座分析

本文基于 MTL 源码、公共头文件、`doc/design.md`、`docs/analysis/` 和 `app/rk3588_*` 参考应用整理，说明它能为 DVS 提供什么、需要在它之上补什么。

## 1. 定位

MTL 是一套基于通用 CPU 和常规网卡的软件 SMPTE ST 2110 媒体传输栈。它把视频、音频、辅助数据封装成 RTP/UDP/IP，按 ST 2110-21 做流量整形，并提供 PTP、组播、冗余和多种数据路径后端。

它解决的是“媒体帧如何实时、守时、低抖动地在 IP 网络上收发”。DVS 的业务对象、窗口模型、场景快照、节点注册和本机后台不在它的范围内。

## 2. 架构

### 2.1 控制面与数据面

| 维度 | 控制面 | 数据面 |
| --- | --- | --- |
| 职责 | 会话创建销毁、配置、统计、ARP/DHCP/PTP/IGMP | 组包、pacing、收包重组、DMA |
| 内存 | libc heap、mutex | hugepage、无锁 ring、spinlock |
| 约束 | 允许阻塞 | 禁止 malloc、mutex、sleep、INFO 日志 |

数据面 tasklet 绑定 CPU 忙轮询，一个 tasklet 阻塞会饿死同核其它会话。节点媒体引擎需要把控制逻辑放到独立线程，热路径只做 get/put 帧。

### 2.2 数据路径后端

| 后端 | 端口名示例 | 适用 |
| --- | --- | --- |
| DPDK PMD | `0000:af:01.0` | 高性能，kernel bypass |
| Native AF_XDP | `native_af_xdp:eth0` | RK3588 推荐，需要 CAP_NET_RAW |
| Kernel socket | `kernel:eth0` | 无 DPDK 网卡时的回退，实验性 |
| DPDK AF_XDP / AF_PACKET | 实验性 | 特定场景 |

RK3588 没有 E810/E830，推荐 `native_af_xdp:` 或 `kernel:`。AF_XDP 需要 `MtlManager` 守护进程和 hugepage。

### 2.3 三层 API

```text
Pipeline（st20p / st22p / st30p / st40p）  get/put 帧，库管理缓冲、色彩转换、编解码插件
      ↓
Session（st20 / st22 / st30 / st40）       回调、slice、RTP 细节
      ↓
Transport（内部）                          组包、pacing、收包重组
```

DVS 推荐使用 Pipeline API，应用代码简单，帧生命周期由库管理。需要行级超低延迟或自定义 RTP 时再接 Session API。

### 2.4 生命周期

```text
mtl_init
  -> st22p_rx_create / st22p_tx_create（可在 mtl_start 前后）
  -> mtl_start
  -> 循环 get_frame / put_frame
  -> mtl_stop
  -> st22p_*_free
  -> mtl_uninit
```

## 3. 协议能力

| 标准 | 能力 | DVS 用途 |
| --- | --- | --- |
| ST 2110-10 | 系统定时、RTP、PTP | 全系统统一时基 |
| ST 2110-20 | 未压缩视频，RFC 4175 | 万兆网络下的低延迟方案 |
| ST 2110-21 | 流量整形，RL/TSC/TSN/PTP/BE | 发送节奏，避免突发丢包 |
| ST 2110-22 | 压缩视频，编解码插件 | 千兆网络下的主方案 |
| ST 2110-30 | PCM 音频 | 窗口音频、场景音量 |
| ST 2110-40 | 辅助数据，RFC 8331 | 时间码、元数据，可选 |
| ST 2022-7 | 双路冗余 | 关键现场的高可用 |
| ST 2022-6 | RTP passthrough | 第三方 SDI over IP 兼容，可选 |

视频格式覆盖 720p、1080i、1080p、4K、8K，帧率覆盖 23.98 到 120，像素格式覆盖 ST 2110-20 全表，包含 YUV 4:2:2 10-bit。Pipeline 内置 SIMD 大小端转换。

## 4. DVS 需要的关键能力

### 4.1 组播

MTL 支持 IGMPv3 组播加入与退出，RX 端可配置组播 IP 和源过滤地址 `mcast_sip_addr`。TX 端把目的地址设为组播 IP 即可。

对 DVS 的价值：一路输入信号只需要发送一份，整墙所有需要该信号的输出盒加入同一组播组。这比逐节点单播节省输入盒出口带宽，也让新增输出节点无需改动发送端。

### 4.2 PTP 与统一时基

MTL 提供两种方式：

1. 内置 PTP：`MTL_FLAG_PTP_ENABLE`，需要网卡支持硬件时间戳。
2. 外部 PTP：应用提供 `ptp_get_time_fn`，通常接 linuxptp 的 `CLOCK_TAI`。

RK3588 参考应用 `rk3588_mp4_dist` 使用外部 linuxptp。RK3588 板载网卡通常没有 PTP 硬件时间戳，软件时间戳在局域网可达百微秒到毫秒级，足以支撑 60 Hz 下 16.7 ms 的帧边界对齐。

PTP 状态需要作为节点能力上报，平台在发起同帧操作前检查同步资格。

### 4.3 VSYNC 与用户 pacing

| 机制 | 作用 |
| --- | --- |
| `ST22P_TX_FLAG_USER_TIMESTAMP` | 应用指定帧时间戳 |
| `ST22P_TX_FLAG_USER_PACING` | 按应用给定时间发送 |
| `ST22P_TX_FLAG_ENABLE_VSYNC` | 每个 epoch 开始回调 `ST_EVENT_VSYNC` |
| `ST22P_RX_FLAG_ENABLE_VSYNC` | 接收侧 VSYNC 事件 |
| `st22p_tx_update_destination` | 运行中切换目的地址，不必重建会话 |
| `st22p_rx_update_source` | 运行中切换源地址 |

这些能力支撑“场景切换不重建会话”和“按全局帧边界提交”。MTL 提供定时原语，prepare/commit 的状态机由 DVS 自研。

### 4.4 冗余与恢复

- ST 2022-7：TX 复制到第二块网卡，RX 双路径去重。适合关键现场。
- RTCP 重传：MTL 私有互操作，非 ST 2110 标准，可选。
- 不完整帧默认丢弃，可配置交给应用。
- 运行时改地址：适合源切换和故障切换。

### 4.5 编解码插件

MTL 的 ST 2110-22 通过插件接口加载编码器和解码器，插件导出 `st_plugin_get_meta`、`st_plugin_create`、`st_plugin_free`。树内 `plugins/st22_avcodec` 基于 libavcodec，适合 x86 评估。

RK3588 上更合适的是 MPP 硬件编解码。参考应用 `rk3588_mp4_dist` 在应用层做 MPP 编解码，再把码流交给 ST 2110-22 会话。这条路径可以直接产品化，也可以封装成 MTL 插件。

### 4.6 可观测性

- `mtl_get_port_stats`：端口级收发、乱序、重复、帧计数。
- 日志级别可调，支持自定义 printer。
- USDT 探针，可用 eBPF 采集。
- pcap dump。
- RX timing parser 可判定 narrow/wide/fail。

这些指标可以进入节点心跳，供平台展示链路质量和排障。

## 5. RK3588 参考应用

### 5.1 rk3588_end2end

链路：

```text
TX: V4L2 采集 -> RGA 色彩转换 -> MPP 编码 -> MTL TX
RX: MTL RX -> slice 重组 -> MPP 解码 -> DRM 渲染
```

| 目录 | 可复用内容 |
| --- | --- |
| `codec/` | MPP 编解码封装、RGA 转换、统一 codec 插件接口 |
| `hal/` | DMA-BUF 分配、帧池、单调时钟、SEI 时间戳 |
| `io/` | V4L2 采集（DMA-BUF 导出）、DRM/KMS 渲染 |
| `pipeline/` | TX/RX 线程编排、环形缓冲、码流切片重组 |
| `mtl_tx/rx` | MTL 会话初始化、收发桥接 |

这个应用适合作为输入盒采集编码链路和输出盒解码显示链路的骨架。

### 5.2 rk3588_mp4_dist

链路：

```text
TX: FFmpeg 解复用 -> Annex-B -> ST2110-22 视频 + ST2110-30 音频
RX: ST22 RX -> MPP 解码 -> 拼板合成 -> DRM 显示；ST30 RX -> AV 同步 -> ALSA
```

对 DVS 最有价值的部分：

| 模块 | 作用 |
| --- | --- |
| `rx/rx_compositor` | 多路 ST22 接收 + 解码 + 单 DRM 多平面输出 |
| `shared/display_layout` | 窗口矩形、层序、多图层布局解析 |
| `rx/playout_buffer` | 播放缓冲，吸收网络和解码抖动 |
| `rx/av_sync` / `rx/media_clk` | 音视频同步、媒体时钟 |
| `shared/ptp_time` | 外部 PTP 接入 |
| `shared/e2e_stamp` / `rx_e2e_stats` | 端到端时延统计 |
| `tx/tx_scheduler` | 按帧调度发送 |

`display_layout` 当前上限为 8 个图层、8 路 PIP，v1 NV12 直通 4 路。DVS 若需要单节点更多窗口，需要增加 GPU/RGA 合成兜底。

### 5.3 应用成熟度

两个 RK3588 应用都是独立 meson 工程，未接入 `app/meson.build`，共享模块各自拷贝。产品化前需要：

1. 抽出公共库：`codec`、`drm`、`ringbuffer`、`slab_pool`、`ptp_time`、`display_layout`。
2. 统一配置模型，增加 revision 和热更新接口。
3. 增加控制通道，接受平台下发的布局片段。
4. 增加状态上报：会话状态、解码状态、显示状态、时钟状态、统计。
5. 补齐单元测试和故障注入。

## 6. 构建与运行约束

| 约束 | 说明 | 应对 |
| --- | --- | --- |
| 主分支定位 | 官方声明主分支用于测试评估，生产使用 tagged release | 固定 release tag，fork 后维护补丁 |
| 依赖版本 | DPDK、ICE、IAVF、FFmpeg、libbpf 版本钉在 `versions.env` | 节点镜像统一版本 |
| hugepage | DPDK/AF_XDP 需要 hugepage | 系统镜像预配置 |
| 权限 | AF_XDP 需要 CAP_NET_RAW；DPDK 需要 VFIO | 用 systemd 能力或专用用户 |
| MtlManager | 多进程资源仲裁，需 root | 节点镜像内置 |
| 无 root 运行 | MTL 支持 | 需要按文档配置 capability |
| ARM pacing | 无 E810 硬件限速，使用 TSC 宽 pacing | 闭环局域网可接受；对接广播设备需评估 |
| 每进程 8 网卡 | 上限 | 节点通常 1 网卡，足够 |
| 代码语言 | C99 核心，C++ 测试 | 节点媒体引擎用 C/C++ |

## 7. 能力边界

| 能力 | MTL | 需自研 |
| --- | --- | --- |
| ST 2110 收发 | 提供 | 会话编排、源切换策略 |
| PTP 时基 | 提供内置和外部接入 | 同步资格判定、异常处理 |
| 组播 | 提供 IGMPv3 | 地址分配、白名单映射 |
| 流量整形 | 提供 | 码率策略、拥塞处置 |
| 冗余 | 提供 ST 2022-7 | 双网部署与切换策略 |
| 编解码 | 提供插件接口 | RK3588 MPP 插件或应用层编解码 |
| 多窗口合成 | 参考应用有 DRM 多平面 | 全局窗口到本地片段拆分、层序、音量仲裁 |
| 场景快照 | 无 | 平台数据模型 |
| prepare/commit | 无 | 平台 operation 服务 + 节点执行器 |
| 节点注册/心跳 | 无 | 节点代理 |
| 节点本机页 | 无 | 节点代理 + 静态页 |
| 固件升级 | 无 | 节点代理 + A/B 分区 |
| 设备管理 | 无 | 平台服务 |

## 8. 集成建议

1. 把 MTL 作为 git submodule 放到 `third_party/Media-Transport-Library`，固定 release tag，不跟随主分支。
2. 建立 `dvs-media` 公共库，收纳 MPP、RGA、DRM、ALSA、PTP、布局、缓冲等模块，输入盒和输出盒共用。
3. 用 MTL 的 Pipeline API 作为唯一媒体 API，避免直接依赖内部 session 实现。
4. 保持控制面与数据面分离：节点代理负责 HTTP/WS 和状态机，媒体引擎只负责帧与设备。
5. 把 MTL 统计和 VSYNC 事件接入节点状态上报，作为同帧资格和排障依据。
6. 对 MTL 的改动以补丁形式维护，记录上游提交，定期评估合并。

## 9. 结论

MTL 与 DVS 的匹配点集中在“采集上 IP、组播分发、PTP 定时、冗余、多路接收”。它可以直接支撑输入盒和输出盒的媒体链路，参考应用已经验证了 RK3588 上的采集、编解码、显示和音频路径。

DVS 的产品差异点集中在控制面和合成策略：全局拼墙坐标、场景快照、跨节点同帧、窗口级音频和节点生命周期。这些需要在 MTL 之上建立平台服务和节点媒体引擎。
