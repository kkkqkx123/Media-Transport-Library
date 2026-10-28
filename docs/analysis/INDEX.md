# Media Transport Library 分析文档索引

分析对象：https://github.com/OpenVisualCloud/Media-Transport-Library

本地副本：当前工作区 `/Media-Transport-Library`

版本：`26.09.0.DEV`（`VERSION`）

许可证：BSD-3-Clause

本目录基于仓库源码、`README.md`、`doc/design.md`、公共头文件与内部实现整理，描述架构设计与已实现功能。

## 文档

| 文件 | 内容 |
|------|------|
| `ARCHITECTURE.md` | 总体架构、控制面/数据面、调度、数据路径、会话分层 |
| `FEATURES.md` | ST 2110 协议能力、 pacing、冗余、生态插件、绑定层 |
| `MODULES.md` | 目录与模块职责、公共 API、关键源文件映射 |
| `AF_XDP_EBPF.md` | AF_XDP 数据面与 eBPF 过滤程序的工作方式，以及 RK3588 应用的集成清单 |
| `PTP_TIME_SYNC.md` | 内置 PTP 从时钟实现：时间源选择、t1–t4 测量、伺服调整（步进/PI/phc2sys）、漂移系数、对外 API 与限制 |
| `RK3588_6.1_XDP_PORT.md` | 把 5.1 XDP 树的 AF_XDP/eBPF 内核使能移植到 RK3588 6.1 SDK（`devs:/data/kkkqkx/rk3588bsp`）的结构、判定与校验 |
| `lib/` | `lib/src/` 根目录逐文件分析（实例/调度/网络服务/支撑服务/平台头），入口见 [lib/README.md](lib/README.md) |

## 相关目录

| 文件 | 内容 |
|------|------|
| `../env/RK3588_BOARD_ENV.md` | **RK3588 开发板（`ssh board`）环境实测记录**：内核 CONFIG/XDP/BPF/AF_XDP 能力、网卡驱动与队列、hugepage、多媒体栈、工具链，附复测探针 `../env/probes/`。改 MTL 板上行为前先读它 |

## 一句话定位

MTL 是一套基于 COTS CPU + 常规网卡的软件 ST 2110 媒体传输栈。它用 DPDK EAL、内核 socket 或 AF_XDP 做数据路径，用 tasklet 调度与 NIC 限速/TSC/TSN pacing 满足 ST 2110-21 微秒级发包节奏，而不依赖专用媒体网卡。
