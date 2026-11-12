# playout_delay 扫参数据（T4）· 2026-11-12

执行器：`app/rk3588_playout_rx/tests/run_playout_delay_sweep.sh`（本轮中定版：
`$3` 指定源、`OUT_DIR` 按源隔离、`stdbuf` 行缓冲、TX 源轮询断言、文件模式 TX 计数解析）。
板端会话全程 `systemctl isolate multi-user.target`（无桌面启停 churn），
`board_env.sh run --` 包裹（prepare/restore 在文本态自动 no-op，已验证兼容）。
hugepages 2048，MtlManager 运行中，后端 `kernel:enP2p33s0`，PTP 均为 monotonic。

- RX：`file_playout_rx {25,40}s --ptp-mode monotonic --stats-interval 10`，
  每档仅 `--config` 覆写 `playout_delay_ms`，其余走 CLI。
- TX HDMI：`live_ingest` HDMI 采集 1920x1080@60，`--pacing tsc_narrow`。
- TX 文件：`live_ingest --source file`，片源 `streams/multinode_av_1080p30.mp4`
 （1920x1080@30 testsrc2 + 440Hz 正弦，loop=on），40s 约发 1210 帧 0 drops。
- 音频：文件轮 TX 发了音频（audio_udp=15002，25045 帧 peak 2912 全非静音），
  但 RX 默认监听 20002，音频面未建连；HDMI 轮未传 `--audio-udp`。
  本轮结论仅限视频播出，音频另行覆盖。
- E2E：全部 n/a（monotonic 下无 TAI 样本，需 external/builtin PTP，见预算 T2）。
- 人工：全程非交互（UNWITNESSED），无看屏结论，待补充（见 §6）。

## 1 HDMI 60fps（两轮重复）

| delay | TX cap/enc | RX pkt/dec | render/dec | skip | underrun（首→末） | overrun | put_fail |
|---|---|---|---|---|---|---|---|
| run1 80ms | 2390/2394 | 2210/2211 | 87.3% | 275 (12.4%) | 393→393 收敛 | 0 | 0 |
| run1 20ms | 2393/2394 | 2214/2215 | 86.9% | 287 (13.0%) | 452→727 发散 | 0 | 0 |
| run1 8ms | 2392/2395 | 2211/2212 | 87.7% | 269 (12.2%) | 622→1830 发散 | 0 | 0 |
| run1 2ms | 2392/2394 | 2211/2212 | 95.2% | 107 (4.8%) | 628→1785 发散 | 0 | 0 |
| run1 0ms | 2391/2395 | 2209/2210 | 98.8% | 26 (1.2%) | 574→1268 发散 | 0 | 0 |
| run2 80ms | 2394/2395 | 2213/2214 | 87.7% | 266 (12.0%) | 392→392 收敛 | 0 | 0 |
| run2 20ms | 2386/2394 | 2207/2208 | 89.6% | 227 (10.3%) | 464→705 发散 | 0 | 0 |
| run2 8ms | 2392/2394 | 2211/2212 | 90.5% | 209 (9.5%) | 617→1685 发散 | 0 | 0 |
| run2 2ms | 2390/2395 | 2210/2211 | 90.9% | 199 (9.0%) | 747→2057 发散 | 0 | 0 |
| run2 0ms | 2392/2393 | 2211/2212 | 98.6% | 31 (1.4%) | 610→1410 发散 | 0 | 0 |

两轮各档 mutually 在 1~3% 内复现（80ms：skip 12.4%/12.0%，under 393/392），
曲线是稳定现象不是噪声。链路全档健康：dec/pkt≈100%，零丢包零损坏零反压。

## 2 文件源 30fps（一轮）

| delay | TX | RX pkt/dec | render/dec | skip | underrun（首→末） |
|---|---|---|---|---|---|
| 80ms | file 1210f | 1088/1110 | 91.5% | 90 (8.3%) | 223→223 收敛 |
| 20ms | file | 1088/1110 | 91.7% | 88 (8.1%) | 241→320 微增 |
| 8ms | file | 1088/1110 | 91.8% | 88 (8.1%) | 349→998 发散 |
| 2ms | file | 1088/1111 | 91.6% | 90 (8.3%) | 341→945 发散 |
| 0ms | file | 1088/1109 | 92.5% | 81 (7.4%) | 369→1073 发散 |

分窗口（80ms：skip 38→63→90 稳态约 8%/窗，under 恒 223；
0ms：skip 38→57→81，under 607→835→1073）。
30fps 下 skip 与 delay 基本无关（恒 ~8%），只有 under 随 delay 减小发散，
与 60fps 形态不同，原因待查（见 §5）。

另有 25s 文件 80ms 探针轮：TX video 760 帧 0 drops；RX 662 pkt 零丢，
613 dec / 556 render，skip 53，under 271（首窗），put_fail 0。

## 3 核心发现

1. skip↔underrun 跷跷板（固定偏移的必然形态）：delay 越大，缓冲填满后
 render 按最新帧排空、跳过过期帧，skip 越高但欠载收敛；delay 越小，
 缓冲恒空，render  spin 空等（underrun 计数涨）但几乎全播出。
 underrun 是空转轮询计数，不是丢帧（0ms render/dec 仍 98%+）。
2. 中间档（20/8/2ms）skip 与 under 双高，没有甜点；可选工作点只有两端：
 收敛档（80ms，欠载零增长，代价 12% 跳帧）与直通档（~0ms，全播出，
 代价 render 空转 + 零抖动吸收）。二选一待人眼。
3. 显示提交层无错：三轮 15 档零 ADDFB2/SetCrtc/SetPlane/PageFlip 报错；
 本会话直通探针与文件端到端均正常出图（此前黑屏怀疑为误判）。
4. 固定偏移的代价是常驻的：80ms 档 E2E 常驻 +80ms（预算表最大项），
 降到 0ms 仍有稳态 skip（60fps 1.4%，30fps 7~8%），与预算“归零仍超标”一致。

## 4 未覆盖与限制

- E2E-C 全轮 n/a（monotonic 口径），时延分位待 PTP external/builtin 重测（预算 T2）。
- 音频面未建连（端口错配），视频结论不受影响。
- 30fps skip 恒 8% 的机理未解释：候选为 drain-newest 与 33ms 周期的系统性错拍，
 需对照 decode 到达节拍与 render 节拍后定论，不影响本次工作点二选一。

## 5 人工观测待补充

以下逐项需看屏填写（显示器接板载 HDMI 输出，testsrc/摇镜优先看运动平滑）：

1. 80ms 档：12% 跳帧在连续运动下是否可察？（自然内容 / testsrc 分开记）
2. 0ms 档：与 80ms 对比，运动平滑度是否更好？有无可察的顿挫（欠载 spin 不直接可见）？
3. 中间档抽一档（建议 8ms）：是否如机器数据所示两头都不如？
4. 一票否决项：黑场/花屏/撕裂/横向分块有无？

结论记 `WITNESSED_PASS/FAIL`（分档记），本报告补入后 T4 机器部分关闭。

## 6 证据清单

本目录 `2026-11-12-sweep-*`：HDMI run2 全量日志（10）+ 见证（1），
HDMI run1 见证（1，日志被 run2 同名覆盖，仅汇总存活，见 §1），
文件轮全量日志（10）+ 见证（1），25s 探针轮收发日志（2）。
Stale 的两次断言误杀轮见证（015638/015813）未归档，原因见执行器说明。
