# docs/tests 使用说明

本目录存放 RK3588 媒体链路的验证设计、验证报告与板端检查工具的使用方法。
设计文档只定分层、拓扑、取证口径与判定，不改生产代码。

## 1 文档一览

| 文档 | 用途 | 何时读 |
|---|---|---|
| `rk3588-tx-rx-e2e-verification-design.md` | 单路全链路验证设计（L0–L7，单板回环与双板互通） | 跑任何板端验证前 |
| `rk3588-tx-rx-e2e-validation-report.md` | 单路单板 60 秒验证报告 | 查单路基线结论 |
| `rk3588-multinode-composite-verification-design.md` | 单盒模拟多节点合成输出验证设计（能证明什么、不能证明什么、未来多盒任务） | 做多节点/合成验证前 |
| `rk3588-multinode-composite-validation-report.md` | 上述设计的板端实测报告（T0–T3 全绿时的数值） | 查多路合成结论 |
| `rk3588-rx-verification-gap-analysis.md` | 接收侧缺口与实现方案（单元/探针/集成/PTP/长稳） | 补单项覆盖时 |
| `rk3588-drm-verification-design.md` | DRM 显示验证设计 | 查显示判定口径 |
| `rk3588-capture-encode-decode-perf-design.md` | 采集编码解码性能画像设计 | 查性能判定口径 |
| `codec-latency-semantics.md`、`codec-perf-test-logic.md` | 编解码时延语义与性能逻辑 | 查编解码判定口径 |

## 2 板端验证脚本

脚本位置：`app/rk3588_playout_rx/tests/`。板端只依赖 `~/code/mtl/bin/` 下的二进制与标准工具，
所需 JSON 配置由脚本自己在输出目录内生成，不依赖外部配置文件。

| 脚本 | 阶段 | 前置 | 执行示例（板端） |
|---|---|---|---|
| `run_tx_rx_e2e.sh` | 单路基线（T0 的长期形态，默认 60 秒） | HDMI 信号、MtlManager | `./run_tx_rx_e2e.sh 60` |
| `run_multinode_composite.sh` | 本阶段主脚本：T0 基线、T1 文件单路、T2 双路合成、T3 配置反例，可单独跑某阶段 | 同上 + 文件片源（默认 `/usr/local/test.mp4`，可用 `FILE_MP4` 覆盖，RX 帧率自动跟随探针） | `./run_multinode_composite.sh all 40` 或 `./run_multinode_composite.sh T2 40` |
| `run_dualboard_interop.sh` | 未来 F1：双板互通 | 第二台板 + 互通网络，本阶段不执行 | 按脚本内环境变量说明准备后执行 |
| `run_dualoutput_sameframe.sh` | 未来 F2/F3：双输出同帧与多节点墙 | 两台输出板 + PTP 域，本阶段不执行 | 同上 |
| `check_playout_log.py`、`check_drm_log.py`、`check_pts_idx.py`、`compare_au_trace.py` | 离线日志检查器 | 日志文件 | 见各脚本头注释 |
| `verify_vrx_margin.py` | ST2110-21 VRX 余量校验（对接专业接收机时） | 抓包/TX 日志 | 见脚本头注释 |

逻辑单测（`test_display_layout`、`test_media_clk`、`test_pacing_config` 等）为交叉编译产物，
直接拷到板端运行即可，无需硬件。

## 3 板端前置与同步

前置：MtlManager 常驻、大页充足、HDMI 输入有时序锁定、显示输出已点亮。
同步方法：仓库改脚本后 `scp` 到板端工作目录即可；二进制变更只在交叉编译服务器上重编，
同步后以 `pkill` 清理残留进程再开测。每轮日志与生成的 JSON 同目录存放，
报告引用末值而非首个窗口（首尾窗口含启停瞬态）。

## 4 判定口径速查

稳态窗口（去头尾）：解码与收包之比、零丢包、零坏帧、零反压；
发送侧跟上目标帧率；文件源供帧量符合片源帧率、零丢弃；
非法配置必须被明确拒绝。渲染数小于解码数属按时钟取最新帧的主动跳帧，
只看趋势与零错误，不做相等断言。绝对时延只在 PTP 域内测量，
单调锚定下仅记录样本有效与偏斜拒绝为零。

## 5 失败定位顺序

先看参数对齐（宽高帧率、端口、收发地址、pacing、码流上限、播放延迟），
再看单路冒烟，最后才怀疑网络或争用。超时不退出先查 feeder 停机路径；
中途检查日志无输出先考虑 stdout 缓冲（脚本已用行缓冲处理）。
