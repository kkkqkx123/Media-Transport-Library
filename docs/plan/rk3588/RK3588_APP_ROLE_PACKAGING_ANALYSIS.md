# RK3588 两个 App 收发角色与打包分析

## 1. 问题

`rk3588_live_ingest` 与 `rk3588_file_playout` 都支持发送与接收（含回环验证）。
需要判断：这种“同时收发”是否有问题，是否应按编码盒/解码盒角色拆分部署包。

## 2. 结论摘要

- 两个 app 支持收发本身不是缺陷，适合开发、联调和回环验证。
- 但把“同时收发”作为节点默认角色/部署形态是有问题的。
- 生产节点角色互斥：输入盒（编码/TX）与输出盒（解码/RX）。
- 应按角色拆分部署包：输出盒已有 `deploy/dvs-node`；输入盒需要独立的 TX-only 媒体引擎与对应包。
- `rk3588_file_playout` 不是 `rk3588_live_ingest` 的子集，两者是并行参考实现，共享基础库与部分 RX 代码。

## 3. 当前实现事实

### 3.1 rk3588_live_ingest

- 单二进制支持 `tx`、`rx`、`both`。
- `both` 模式实际 fork 出 TX/RX 两个进程，因为 DPDK EAL 不能在同一进程重复初始化。
- 能力链路：V4L2 采集 → RGA → MPP 编码 → MTL TX；MTL RX → MPP 解码 → DRM。
- 没有音频 ST2110-30 链路，也没有 MP4 文件源。

### 3.2 rk3588_file_playout

- TX/RX 是两个独立可执行文件：`file_playout_tx`、`file_playout_rx`。
- `both` 仅由脚本先起 RX，再起 TX。
- TX 可在 x86 编译；RX 需 RK3588 + MPP + DRM + ALSA。
- 能力链路：MP4 解复用 → ST2110-22 视频 + ST2110-30 音频 TX/RX；RX 含 MPP 解码、DRM 显示、ALSA 播放、多 lane 合成。
- `dvs_media` 由其 RX 侧演进而来的生产媒体引擎。

## 4. “同时收发”的问题

1. 资源与实时性冲突：同板 TX+RX 争用 lcore、队列、hugepage、编解码器和显示资源，可能破坏 ST 2110 的微秒级 pacing。
2. 角色语义不清：平台/代理把节点定义为输入盒或输出盒，一个同时收发的包无法表达唯一角色。
3. 控制面不对称：`dvs_agent`/`dvs_media` 当前只驱动输出盒（RX），输入盒 TX 尚未被控制面编排。
4. 打包边界模糊：`deploy/dvs-node` 已定义输出盒为 agent+media 同包；输入盒若混入同一 app/包，版本契约、启停、回滚会混杂。
5. 文档与实现漂移：`docs/app/rk3588_live_ingest.md` 曾描述 `both` 为同一进程内双栈，但代码实际 fork 两个进程。

## 5. 是否应按角色拆包

是。建议：

- 保留 `rk3588_live_ingest` 与 `rk3588_file_playout` 作为参考实现和实验室回环工具，继续支持 `tx/rx/both`，但明确仅用于开发/验证。
- 生产输出盒继续使用 `deploy/dvs-node`（`dvs_agent` + `dvs_media`，RX-only）。
- 为输入盒新增独立的 TX-only 媒体引擎，从 live_ingest 的采集/编码/MTL TX 链路或 file_playout TX 演进，并与 agent 同包发布。
- 生产镜像只包含对应角色的二进制，不交付 `both` 模式。
- 角色切换时按角色启动/停止对应引擎，避免同一节点同时运行 TX 与 RX 媒体链路。
- 修正文档中 `both` 模式的描述，明确为 fork 两个进程的回环验证。

## 6. rk3588_file_playout 是否为 rk3588_live_ingest 的子集

不是。

两者是并行的参考实现，共享 `rk3588_common` 与部分 RX 代码，但能力集合不互相包含：

- `rk3588_live_ingest` 独有：V4L2/HDMI 采集、RGA 色彩转换、MPP 编码、TX 侧帧级码流写入。
- `rk3588_file_playout` 独有：MP4 解复用、ST2110-30 音频、多 lane 合成、输出盒显示/音频播放、外部 PTP 时间轴、可 x86 编译的 TX。
- `dvs_media` 只复用 `rk3588_file_playout` 的 RX 侧，不复用 live_ingest。

因此两者不是子集关系，而是不同场景的端到端参考实现。
