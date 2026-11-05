# dvs_agent 与 dvs_media 不合并、不引入 epoll 框架：架构决策

> 结论收敛自 `docs/analysis/dvs_agent_media_separation.md`（进程分离合理）与
> `docs/analysis/RK3588_APP_MERGE_ANALYSIS.md`。
> 本篇只记录“参照 ZLMediaKit 后为什么仍不合并、不引 epoll”，不重复上述两篇的论证细节。
> 若知识库与代码矛盾，以代码为准。

## 背景

参照对象为 `~/code/zlmediakit` 的单进程模型：`ZLToolKit EventPoller（epoll）+ ThreadPool/WorkThreadPool`
驱动 `TcpServer/UdpServer`，`MediaSource/MultiMediaSourceMuxer` 做流注册表，
`Rtmp/Rtsp/WebRTC/SRT` 等协议共享线程池，`HdmiIn` 按采集、转换、编码、媒体源、协调器分层后发布为普通媒体源。

被评估的改动是：把 `app/dvs_agent` 与 `app/dvs_media` 合并为单进程，
用多线程模型统一管理编解码，并引入 epoll 框架统一驱动网络与媒体。

## 实测与现状盘点

- 当前已是多线程，不缺线程模型：接收侧每个通道有独立解码线程加一条渲染线程，
  发送侧有编码线程加监管线程，引擎另有统计循环线程，IPC 服务端按连接设读线程、
  耗时准备走独立工作线程，代理侧有 HTTP 服务线程与注册心跳线程。
- 数据面本质是轮询而非事件就绪：视频每帧数千包、微秒级 pacing，
  靠 MTL 调度器的 tasklet 紧循环、共享队列与 DMA 推进，不是文件描述符可读事件。
- 控制面并发量级极低：代理 HTTP 主要是本地管理页与平台心跳，媒体 IPC 上限个位数连接、
  行协议 JSON，现有阻塞加 poll 实现已够用，不存在 C10K 场景。
- 构建约束不对称：代理只依赖 json-c 与内置 SQLite，可本地构建测试；
  媒体依赖 MTL、MPP、DRM、ALSA、FFmpeg，只能交叉编译。合并后轻量可测性丢失。
- 线程亲和已有配置项：解码、渲染、桥接、采集、编码均可通过配置指定 CPU，
  落点分散在收发两条管线与引擎编排中，缺的是命名与收敛，不是缺框架。

## 决策

1. 不合并进程。保持 `dvs_agent（控制面）` 与 `dvs_media（媒体引擎）` 双进程，
   通道保持现状：平台到代理走 HTTP，代理到媒体走 Unix socket 行协议，
   同包发布加 hello 握手维持配对契约。
2. 数据面不引入 epoll/EventPoller。MTL 轮询语义与 epoll 阻塞语义不兼容，
   引入只会增加唤醒与拷贝，不提升 pacing 精度。
3. 控制面不引入 ZLToolKit 整套线程与网络库。为个位数连接引入通用高并发框架，
   体积与维护成本净增，且与现有 `dvs_common` 的轻量 HTTP 实现重复。
4. 编解码线程模型不推倒重来，只做原模块内收敛（命名、亲和默认值、工作队列边界），
   详见 `docs/plan/dvs` 下对应的借鉴点设计。

## 不从 ZLMediaKit 搬的东西与理由

- 单进程 MediaServer 形态：搬过来即失去故障隔离（媒体崩溃拖垮注册与心跳）、
  独立重启与升级能力，以及数据面与控制面的进程级 two-world 隔离。
- EventPoller 全量网络栈：解决的问题在本节点不存在，且会把阻塞调用习惯带入数据面。
- 通用编解码与协议复用层：本节点编解码必须是 MPP 硬件路径，传输必须是 ST 2110，
  ZL 的软件编解码与互联网协议栈无一可直接复用。

## 从 ZLMediaKit 借的三点（只借思想，不搬代码）

- 媒体源注册表思想：将会话准入、白名单、通道映射与状态快照收敛为统一的会话视图，
  供状态查询与屏障核对使用，改动只放在现有会话与引擎模块内部。
- 任务执行器思想：IPC 侧用有界工作队列承载提交与清理等重操作，读线程只负责收发，
  不引入外部线程池库。
- 管线分层思想：采集、转换、编码、媒体源、协调器的职责划分与现有收发管线已同构，
  保持 donor 持有数据面、引擎只做编排的分工，不新建管线模块。

## 参考

- `docs/analysis/dvs_agent_media_separation.md`
- `docs/analysis/RK3588_APP_MERGE_ANALYSIS.md`
- `docs/plan/dvs/DVS_AGENT_MEDIA_IPC_FOLLOWUP_ANALYSIS.md`（通道分离与订阅属体验优化层，依赖包络稳定之后再做）
- ZLMediaKit 源码：`3rdpart/ZLToolKit/src/Poller/EventPoller.h`、`src/Common/MediaSource.h`
