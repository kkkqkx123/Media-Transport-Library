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
| `ZIP_VS_UPSTREAM.md` | zip 改造版 vs 官方 26.09 差异，以及零修改/最小修改方案 |

## 一句话定位

MTL 是一套基于 COTS CPU + 常规网卡的软件 ST 2110 媒体传输栈。它用 DPDK EAL、内核 socket 或 AF_XDP 做数据路径，用 tasklet 调度与 NIC 限速/TSC/TSN pacing 满足 ST 2110-21 微秒级发包节奏，而不依赖专用媒体网卡。
