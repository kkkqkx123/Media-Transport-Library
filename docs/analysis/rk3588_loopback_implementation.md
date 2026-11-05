# RK3588 应用本机回环实现说明

当前回环验证不再使用单进程 `both` 模式，而是分别启动编码发送端与解码接收端两个进程，经网络互通。

## rk3588_live_tx（TX-only）

入口：`./build/live_ingest [秒] [--config ...]`

- 单进程 TX：V4L2 采集 → RGA → MPP 编码 → MTL TX (ST2110-22)；或文件帧源插件。
- 可选 `--source file --mp4 FILE` 发送文件帧源，音频走 ST30。
- 无接收/解码/显示能力，回环需配合 `rk3588_playout_rx`。

## rk3588_playout_rx（RX-only）

入口：`./build/file_playout_rx [秒] [--config FILE]`

- 单进程 RX：MTL RX → MPP 解码 → playout → AV sync → DRM 显示 / ALSA 播放。
- 参考流参数探针使用 `InTheGrey.mp4`；实际帧率/分辨率需与发送端对齐。

## 单板回环

1. 先启动 `file_playout_rx`（接收进程）。
2. 等待约 2 秒后启动 `live_ingest`（发送进程）。
3. 两侧端口、IP、帧率、分辨率必须匹配；TX 目的地址/端口与 RX 监听地址/端口一致。
4. 可使用 `rk3588_playout_rx/tests/run_single_board_plan_a.sh` / `plan_a_verify.sh` 辅助验证。

## 共同点

- 两个独立进程通过网络栈回环，不是单进程双栈；DPDK EAL 不能在同一进程重复初始化。
- 都用于开发、联调和回环验证，不作为生产节点默认运行形态。