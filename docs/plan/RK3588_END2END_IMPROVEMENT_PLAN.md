# RK3588 End2End App 采集/编码链路改进设计方案

对照参考实现：ZLMediaKit `src/HdmiIn/`（V4L2 采集 → RGA 转换 → MPP 编码 → 分发）。
被改进对象：`app/rk3588_end2end/`（`io/v4l2_capturer.c`、`codec/{mpp_encoder,rga_converter}.c`、`pipeline/tx_pipeline.c`）。

## 1. 现状对照总结

| 维度 | rk3588_end2end 现状 | zlmediakit HdmiIn 做法 | 差距等级 |
|---|---|---|---|
| 格式协商 | S_FMT 尝试 NV12→BGR3，不回读校验 | 候选清单逐个 S_FMT+G_FMT 回读校验，占位分辨率视为无信号 | 高 |
| 信号/热插拔 | 无（设备节点固定、参数固定） | 订阅 V4L2_EVENT_SOURCE_CHANGE，事件驱动全量重建 | 高 |
| 时间戳 | 用户态 DQBUF 时刻 clock_gettime | 内核帧时间戳优先 + 缓存时钟偏移换算 + 单调保护 | 高 |
| 缓冲归还 | 回调消费后立即 QBUF，无防御 | 代际+认领位原子状态表，恰好一次归还；BufferLease 多消费者计数 | 高 |
| 背压/丢帧 | ring 满丢帧，仅计数 | 在途缓冲观测、水位 streak、三级 FPS 门控、PreLag 省算力 | 中 |
| RGA buffer 池 | 固定池环形轮转 `next_buf++ % N` | 主/子独立池、锁内原子取用、失败回推、泄漏对账 | 中 |
| RGA 错误处理 | blit 失败仅打日志丢帧 | 连续失败≥3 降级 VA 路径；启动期能力探测粘滞禁用 | 高 |
| MPP 编码模式 | 单线程内 put_frame + 忙等 get_packet | Feeder/Collector 双线程 + 有界 FrameRing，异步管线 | 高 |
| 编码挂死保护 | 无（10ms 重试后放弃该帧） | HangGuard + 3s 看门狗，区分采集停摆 vs 编码挂死 | 高 |
| 参数热更新 | 无（set_param 返回 ENOSYS） | RC 热重配不断流；配置分级生效（热配/重建/告警） | 中 |
| 色彩空间 | RGA 输出侧写 BT.709，无 VUI 注入 | 编码后 SPS 位流级注入 VUI（full_range/709），纯函数可单测 | 高（色偏） |
| 错误恢复 | 采集线程 DQBUF 失败直接 break 退出 | 错误分类 → 防风暴重建 → 超限禁用 | 高 |
| 可观测性 | 4 个简单计数器 | 原子计数全覆盖 + EMA 耗时 + 30s 周期日志 + 告警限频 | 中 |
| 生命周期顺序 | free 中 stop 后立即 munmap/close | Stop 先 join 后关句柄；mmap/fd 延迟释放 | 中 |
| 热暂停 | 无 | SuspendForPause 保 fd，恢复免重开设备 | 低 |

## 2. 改进方案

### 2.1 采集层（v4l2_capturer.c）

**P1 格式协商回读校验**
`S_FMT` 后必须 `G_FMT` 回读 fourcc/分辨率/stride，与请求值不一致则按候选清单
（NV12 → BGR3 → UYVY）继续尝试，全部失败才报错。分辨率 ≤128×128 视为无信号
占位，返回专用错误码（`-ENODATA`）由上层周期重试，而不是带病启动。

**P1 信号变化检测**
采集循环由纯 DQBUF 改为 `poll(POLLIN|POLLPRI)`，订阅 `V4L2_EVENT_SOURCE_CHANGE`。
事件到来时只置一个原子标志并回调 `on_signal_change`，不做任何重建动作（回调内
不做重活，与 HdmiIn 同一纪律）。上层收到后走"停管线 → 重建采集+编码器"路径。

**P1 内核时间戳优先**
DQBUF 时读取 `buf.timestamp`（配合 `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`）替代
用户态 `gettime_ns()`。V4L2 内核时间戳是 ISP 打戳时刻，早于用户态 DQBUF 数毫秒，
对 MTL 发送端打包时间轴（以及与音频对齐）精度收益直接。保留 DQBUF 时刻作为
fallback，并加"PTS 不倒退"单调钳制。

**P1 恰好一次归还（代际 + 认领位）**
每个 buffer 维护一个 `atomic_uint64_t release_state`：低 1 位认领位、高位帧代际。
DQBUF 时代际+1、清认领位；现在"回调后立即 QBUF"改为"回调持有 dmabuf fd 期间
不归还，编码线程用完（send 后 MPP 已 import / RGA blit 已完成）再通过 release
闭包 CAS 归还"。这消除当前隐患：capture 回调同步调用 QBUF，若下游 RGA/MPP
仍在异步使用该 dmabuf（MPP import 是异步消费），驱动复写该缓冲会产生花屏。
落点：`enc_send()` 在 `mpp_buffer_import` 后即返回，MPP 内部仍持有 fd——当前代码
下一帧就 QBUF 回驱动，是真实的竞态窗口。

**P2 在途缓冲观测与背压告警**
原子计数 `_outstanding/_peak_outstanding/_full_events`（DQBUF 后未归还数），
超阈值打限频告警（5s 冷却），为排障与后续调 buf_count 提供数据。

**P2 错误恢复路径**：poll/DQBUF 非瞬态失败 → 回调 `on_error`，上层按
"防风暴重建（连续 N 次内重建，超限禁用并告警）"处理；EAGAIN/EINTR 静默重试。

### 2.2 RGA 层（rga_converter.c）

**P1 缓冲归还与池语义**：将环形轮转改为带占用标志的池（或直接复用 2.1 的
release 闭包），blit 完成不等于编码消费完成；池耗尽时丢弃新帧并计数，而不是
隐式复用可能仍被 MPP 引用的 buffer。

**P1 失败降级链**：blit 失败当前直接丢帧。改为：单次失败丢帧+计数；同一路径
连续失败 ≥3 次切换 mmap+CPU 拷贝回退（编码器已有该 fallback，RGA 层目前没有），
并粘滞记录降级状态；重建时重新探测。

**P2 DMA32 堆分配**：当前 `c_RkRgaGetAllocBufferCache` 分配的 buffer 可能落在
4GiB 以上物理地址，触发 `rga_mm: unsupported memory larger than 4G`。参考
`HdmiInDma32`：改用 `/dev/dma_heap/system-uncached-dma32` 分配 NV12 缓冲，
`mpp_buffer_import(EXT_DMA)` 导入，RGA 输出与 MPP 输入共用同一 fd，全程零拷贝。
CPU 访问场景补 `DMA_BUF_IOCTL_SYNC` RAII 同步。

**P2 监控**：calls/fails/time-EMA 按路径（fd 路径/VA 路径）原子统计，接入统一
统计上报（见 2.5）。

### 2.3 编码层（mpp_encoder.c）

**P1 异步化：Feeder/Collector 双线程**
现状 `enc_send` → 忙等 `enc_recv` 在同一线程串行，MPP 编码耗时直接挤占采集/
转换窗口（1080p60 帧周期 16.7ms，重试上限就占 10ms）。改为：
- Feeder（现 encode_thread）只做 `encode_put_frame`，put 完即取下一帧；
- Collector 线程循环 `encode_get_packet`（有界超时，唯一超时入口），凑齐 EOI
  后写入有界 ring（深度 8，满即丢 + FullDrop 计数）；
- tx_pipeline 从 ring 取包做 SEI 注入与 MTL 投递。
`enc_recv_into` 的动态 wait 时长（冷启动 30ms/常规 20ms）和 `enc_drain_discard`
的隐式丢包行为随之消除。

**P1 挂死保护（HangGuard）**
Collector 维护 `last_output_ns`；监督逻辑（可挂在 tx_pipeline 主循环或独立
timer 线程）周期检查：`now - last_output > hang_threshold` 且采集侧仍有新帧
供给 → 判定编码挂死 → 销毁重建编码器；采集停摆（无新帧供给）则走采集层重建。
连续重建超限（如 5 次）粘滞禁用并告警。启动空窗期（`frames_out < 5`）不计入。

**P1 SPS VUI 注入（修色偏）**
当前 RGA 输出按 BT.709 full-range 转换，但 MPP 1.5.0 无法配置 `h264:vui_*`，
SPS 无 VUI，接收端/播放器按 BT.601 limited 解释 → 色偏。参考 `HdmiInSpsVui`：
在 tx_pipeline 投递前对关键帧 SPS 做位流级 VUI 注入
（video_full_range_flag=1, colour_primaries/transfer_characteristics/matrix_coefficients=1），
解析失败保持原包不破坏码流。抽成无硬件依赖的纯 C 模块（`codec/sps_vui.c`），
可进 `tests/` 单测（对应 zlmediakit 可 x86 单测的设计）。

**P2 set_param 落地 RC 热重配**
`codec_plugin.set_param` 当前返回 `-ENOSYS`。实现 RC 子集（bitrate/gop/qp）：
`MPP_ENC_SET_CFG` 支持运行中重配且不断流；调用点（如自适应码率、控制面）异步
投递到编码线程串行执行，避免与 put_frame/get_packet 并发。

**P2 编码质量统计**：从 MppPacket meta 读取 QP/码率信息，按秒聚合上报。

### 2.4 编排层（tx_pipeline.c）

**P1 回调纪律与线程模型**
`capture_on_frame` 目前在采集线程内做 malloc、时间戳获取、ring push——基本
符合"回调只投递"纪律，但 `time_now` 回调若指向 MTL PTP 查询可能持锁阻塞采集。
约束并注释：采集回调内禁止任何可能阻塞调用；PTP 时间戳在入 ring 前采样一次，
编码线程不得二次采样覆盖（现状 cap_ts 在编码线程有 fallback 采样，混用两个
时钟域，应统一为采集时刻单一来源）。

**P2 帧供给决策点收敛**：将"何时丢帧"收敛到入 ring 一处（ring 满即丢 + 计数），
编码线程内的重试循环去掉（异步化后自然消除），避免多处独立丢帧导致统计口径
混乱。

**P2 统计汇聚与周期日志**：`stat_captured/encoded/ring_drops` 扩展为统一
stats 结构（各层原子计数 + 水位），tx_pipeline 每秒聚合一条日志，字段覆盖：
采集 fps/丢帧、在途缓冲峰值、RGA 调用/失败/耗时、编码输出/挂死计数、ring 水位。

### 2.5 明确不引入的部分

以下 HdmiIn 机制与本 app 场景不匹配，暂不采纳，留档避免过度设计：
- **多路子码流/按需启停（SubStream/SubDemandController）**：本 app 单码流直送
  MTL TX，无多读者场景。
- **暂停管理（PauseManager）**：ST 2110 发送持续运行，无读者事件概念。
- **FPS 门控（FpsGate）**：HDMI IN 源帧率即目标帧率（60→60），无重采样需求；
  仅采纳"欠速源不重排到目标网格"的原则性检查（当前发送按帧驱动，天然满足）。
- **WebApi/mINI 配置热重载框架**：本 app 用静态 config 文件，仅需 2.3 的 RC
  热重配能力，不引入配置框架。
- **OSD 合成**：与 MTL 超低延迟发送目标冲突，不在范围。

## 3. 实施顺序

| 阶段 | 内容 | 依赖 |
|---|---|---|
| 1 | 2.3 SPS VUI 注入（独立、可单测、直接修色偏） | 无 |
| 2 | 2.1 格式回读校验 + 内核时间戳 + 归还代际 | 无 |
| 3 | 2.3 编码异步化（Feeder/Collector + FrameRing） | 依赖阶段 2 的归还语义 |
| 4 | 2.2 RGA 池语义 + DMA32 堆 + 降级链 | 阶段 3 |
| 5 | 信号变化检测 + 错误恢复 + HangGuard 看门狗 | 阶段 3 |
| 6 | 统计汇聚 + RC 热重配 | 阶段 4 |

阶段 1–2 相互独立可并行；3 是最大的结构改动（消除忙等，收益最大）；4–6 增量叠加。

## 4. 验证方式

- 纯逻辑模块（sps_vui、归还状态机、帧率判定）进 `app/rk3588_end2end/tests/`，
  参照 `test_codec.c` 既有 gtest 风格。
- 每阶段用 `docs/app/README.md` 的 E2E 流程在板上跑 24h 长稳，观察统计日志：
  丢帧率、在途缓冲峰值、RGA/编码失败计数均应为 0 或有界。
- 色偏验证：阶段 1 后用彩条/HDMI 信号源比对接收端渲染与源端色彩。
