# 单一 media 包 + 运行时角色选择的可行性分析

> 分析对象：`app/dvs_media` 当前实现、`app/rk3588_live_tx` donor chain、`app/rk3588_playout_rx` donor chain
> 结论先行：**从架构与工程可行性看，最终节点应采用单一 `dvs_media` 包并在运行时选择 input/output 角色；但这不意味着合并 donor 应用，而是把两类数据面能力收敛到同一个受控媒体引擎中。当前代码已经朝这个方向前进（`HAVE_TX_BACKEND=1`、`dvs_engine_set_role`、`file_playout_mtl_init_dual`），剩余工作集中在角色切换的完整 teardown、能力门控和验证。**

---

## 1. 为什么运行时切换比固定角色包更合理

1. **节点角色不是硬件天性**：同一 RK3588 板卡可能具备 HDMI 输入与输出能力，角色由部署与配置决定。
2. **运维要求角色切换**：需求与计划都包含角色切换作业、影响检查、平台清理、重启与注册验证。固定角色包只能通过替换二进制或重装镜像实现，成本高且容易错配。
3. **单包更利于版本一致性**：`dvs_media` 与 `dvs_agent` 同包发布，IPC 版本与 Bundle 版本 already 要求配对。一个 media 二进制减少版本矩阵。
4. **包大小不是主要约束**：RK3588 依赖（MTL、MPP、RGA、DRM、ALSA、FFmpeg）在节点镜像中 anyway 需要安装；多编译几 MB 代码不是瓶颈。
5. **运行时切换可以避免进程重启**：如果 MTL 与 DRM/V4L2 资源生命周期设计得当，角色切换可以在同一进程内完成，减少中断与恢复时间。

---

## 2. 当前代码是否已经支持

基本支持，且已经接入 TX donor：

- `dvs_media/meson.build` 已 in-tree 编译 `rk3588_live_tx` 的 capture/encode/TX donor，并定义 `HAVE_TX_BACKEND=1`。
- `engine_ctx.c` 已包含 `tx_backend_live_ingest.c`，并调用 `dvs_tx_backend_set_ctx`、`dvs_tx_backend_entry`。
- `dvs_engine_set_role()` 已实现 `input` / `output` 两种角色。
- `dvs_engine_init_mtl()` 与 `dvs_engine_apply_layout_at()` 使用 `file_playout_mtl_init_dual()`，说明 MTL 初始化已经按双端口/双角色设计。
- `dvs_engine_tx_start()` 在启动 TX 前会释放 RX compositor，体现 role 互斥。

因此，"单包运行时选择"不是未来设想，而是当前实现方向；剩余的是把这条路径做完整和可靠。

---

## 3. 还缺什么

### 3.1 角色切换 teardown

- `dvs_engine_set_role("output")` 当前只停止 TX，但需要确保 live_ingest 的 `tx_pipeline`、`mtl_tx`、V4L2、RGA、MPP encoder 全部释放。
- `dvs_engine_set_role("input")` 当前会清掉 RX layout，但需要确保 RX compositor、解码器、播放缓冲完全释放，避免残留 DRM plane。
- 切换后 MTL 实例必须保持可用，不能重复 `mtl_init`。

### 3.2 能力门控

- 不是所有节点都具备 HDMI 输入或输出。`capability.c` 需要真实探测 V4L2 输入口、DRM connector/plane、编解码能力。
- 角色切换前应校验目标角色所需能力；不支持时返回明确错误，而不是启动后失败。

### 3.3 资源隔离

- input 角色下 RX compositor 已停，但 DRM device 可能仍被 RGA/DMA 使用；需要确认切换后资源可重新分配。
- output 角色下 TX pipeline 已停，但 V4L2/RGA/MPP encoder 资源需要完全释放，避免长期运行内存增长。

### 3.4 状态与审计

- 角色切换应写入 applied revision、能力快照和审计事件。
- agent 需要把角色切换结果上报平台，平台才能更新节点期望状态。

### 3.5 验证

- 需要覆盖 `output -> input -> output` 往返切换，验证无 MTL 双 init、无资源泄漏、无残留 plane。
- 需要验证 capability 不足时的拒绝路径。

---

## 4. 单包运行时选择的推荐架构

推荐最终架构为：

```text
dvs_media (single binary)
  ├── output role: file_playout RX data plane
  ├── input role: live_ingest capture/encode/TX data plane
  ├── shared MTL instance (rx port + tx port)
  ├── capability-driven role gating
  └── IPC-controlled role switch with full teardown
```

关键原则：

1. **编译期包含两类能力**，运行期只激活一类。
2. **MTL 一次初始化，双端口可用**，避免角色切换时重建 MTL。
3. **role 互斥在数据面层执行**，不仅在状态机层标记。
4. **donor 应用保持独立**，用于链路验证与回归，不作为生产部署包。
5. **能力不足时拒绝切换**，不尝试"尽力运行"。

---

## 5. 与"合并 donor 应用"的区别

单包运行时选择不等于合并 donor 应用：

- `rk3588_live_tx` 和 `rk3588_playout_rx` 仍然是独立验证应用，保留端到端测试入口。
- `dvs_media` 是生产集成点，in-tree 复用 donor 模块，不复制源码。
- donor 应用的测试矩阵继续存在，用于定位数据面问题。

因此，前一份分析中"不合并应用"的结论仍然成立；但生产节点最终应使用一个支持运行时角色选择的 `dvs_media`，而不是分别发布 TX-only 和 RX-only media 二进制。

---

## 6. 实施建议

1. 完成 `dvs_engine_set_role()` 的真实 teardown，覆盖 RX compositor 与 TX pipeline。
2. 让 `file_playout_mtl_init_dual()` 在所有角色下都初始化 RX 与 TX port。
3. 完善 `capability.c` 的输入/输出能力探测，并在角色切换前校验。
4. 为 `dvs_engine_tx_start/stop` 增加幂等与重复调用保护。
5. 增加往返切换测试与资源泄漏检查。
6. 更新部署文档：节点统一安装 `dvs_media`，角色由配置/IPC 决定。

---

## 7. 结论

**最终架构建议采用单一 `dvs_media` 包 + 运行时角色选择。**  
这与节点运行时切换需求一致，也与当前代码方向一致。保留 donor 应用作为验证入口，不把它们合并为一个应用；真正合并的是两类数据面能力，最终收敛到同一个受控媒体引擎。
