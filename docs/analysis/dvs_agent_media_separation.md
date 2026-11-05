# DVS Agent 与 Media 分离分析

## 问题

Agent 与 Media 分离的理由是否成立？是否可以用多线程在同一进程内实现相同效果？

## 结论

- 在功能层面，多线程可以实现配置下发与状态上报。
- 在生产可靠性、可运维性、实时性边界和故障隔离层面，多线程不能等价替代进程分离。
- 因此 Agent 与 Media 保持进程分离是合理的。

## 分离解决的问题

1. 故障隔离：Media 接触 DPDK/MPP/DRM/ALSA，崩溃风险更高；分离后 Media 崩溃不会直接拖垮 Agent。
2. 生命周期与升级：Media 可以单独重启、替换或升级，Agent 保持注册、心跳和本地管理在线。
3. 实时性与调度隔离：Media 数据面轮询且延迟敏感；Agent 控制面包含阻塞 I/O、数据库和 HTTP。进程边界更容易隔离 CPU、优先级和内存策略。
4. 强制 two-world rule：数据面不能调用控制面阻塞代码；进程边界比线程纪律更可靠。
5. 安全与权限边界：Media 需要硬件/hugepage/网卡等特权资源；Agent 对外暴露 HTTP API。分离可限制各自权限与攻击面。
6. 配对与运维契约：`dvs-node` 通过 Bundle 版本和 IPC 版本校验 Agent/Media 配对；分离进程使契约清晰可验证。

## 多线程方案的不足

- Media 崩溃会拖垮整个节点。
- 无法独立重启或升级 Media。
- 共享地址空间增加对象生命周期和内存管理复杂度。
- 容易把控制面阻塞调用带入数据面。
- DPDK/MTL 的进程模型与hugepage/lcore假设在混用场景下更难排障。

## 建议

- 保持 Agent 与 Media 进程分离。
- 保持 HTTP 作为 Platform-Agent 通道，Unix socket 作为 Agent-Media 通道。
- 通过 Bundle 版本与 IPC 版本维持配对契约。
- 扩展能力时优先扩展消息契约、超时和重试语义，而不是合并通信通道或进程。
