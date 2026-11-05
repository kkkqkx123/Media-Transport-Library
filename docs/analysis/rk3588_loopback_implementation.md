# RK3588 应用本机回环实现说明

## rk3588_live_ingest

入口：`./build/live_ingest both [秒] [--config ...]`

1. 父进程 fork RX 子进程，先运行 `run_rx_mode`：
   - 使用 `rx_port/rx_lip` 单独初始化 MTL（RX 队列）。
   - 创建 `mtl_rx`、RX bridge、`rx_pipeline`、MPP 解码、DRM 渲染。
   - 监听配置的 UDP 端口并显示解码结果。

2. 父进程 fork TX 子进程，`sleep(2)` 后运行 `run_tx_mode`：
   - 使用 `tx_port/tx_lip` 单独初始化 MTL（TX 队列）。
   - 采集 HDMI、RGA 转换、MPP 编码，再经 MTL TX 发送。
   - 无信号时重试。

3. 父进程等待两个子进程结束并汇总结果。

## rk3588_file_playout

入口：`./tests/file_playout_both.sh [秒] [额外参数...]`

1. 先启动 `file_playout_rx`：
   - 监听 `rx_port/rx_lip` 和视频/音频 UDP 端口。
   - 接收 ST2110-22/30，MPP 解码，DRM 显示，ALSA 播放。

2. 等待 2 秒后启动 `file_playout_tx`：
   - 从 MP4 解复用 H.264 和 PCM。
   - 经 MTL 发送 ST2110-22 视频与 ST2110-30 音频。

3. 脚本等待并汇总退出状态。

## 共同点

- 都是两个独立进程通过网络栈回环，不是单进程双栈。
- TX 目的地址/端口必须与 RX 监听地址/端口匹配。
- 都用于开发、联调和回环验证，不作为生产节点默认运行形态。
