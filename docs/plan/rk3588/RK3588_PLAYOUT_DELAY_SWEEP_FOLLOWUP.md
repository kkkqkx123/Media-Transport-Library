# playout_delay 扫参：文档关系与验证后步骤

定位：本方案是预算文档 `RK3588_1080P_SUB10MS_E2E_BUDGET_AND_THREAD_MODEL.md`
§5 T4 的执行与收尾说明，回答“扫参跑完后怎么定 delay、改什么、归档到哪”。
具体敲什么命令只看 runbook，不在此重复。

## 1 相关文档关系

| 文档 | 职责 |
|---|---|
| `RK3588_1080P_SUB10MS_E2E_BUDGET_AND_THREAD_MODEL.md` | 判定依据：playout 降到零仍超标（典型约 10.1ms、最坏约 14.8ms），所以扫参只定可用下限，不证 <10ms 成立；T1/T2/T5/T6 另行推进 |
| `docs/tests/rk3588-manual-verification-runbook.md` §3/§4 | 唯一动手入口：扫参脚本的同步、一条龙命令、每档看屏检查单 |
| `docs/tests/manual-scripts-inventory.md` §4 | 脚本与板端位置索引 |
| `app/rk3588_playout_rx/tests/run_playout_delay_sweep.sh` | 执行器：逐档覆写 `playout_delay_ms`（默认 `80 20 8 2 0`），机器汇总 + 人眼见证同落 `OUT_DIR` |
| `docs/tests/rk3588-capture-encode-decode-perf-design.md`（抖动缓冲段）、`codec-perf-test-logic.md`（`pb_underrun` 告警）、`rk3588-rx-verification-gap-analysis.md`（delay 曲线缺口） | 设计口径：欠载/跳帧的定义与门限来源 |
| `tests/perf/common/thresholds.json` | 阈值唯一收敛点，不在脚本里硬编码 |

默认值现状：`file_playout_config.c` 中 `playout_delay_ms=80`，`--config`
只叠加该字段；CLI 保持现状，不为扫参加临时参数。

## 2 扫参产出

每轮产出三件套，全部进 `OUT_DIR`（默认 `/tmp/playout_sweep`）：各档发送/接收日志、
见证文件 `playout_sweep_witness_*.log`（每档人眼结论 + 机器汇总行）。
HDMI 与文件源各跑一轮：前者代表生产链路，后者运动连续、抖动结论更稳。

## 3 验证后步骤

1. 收敛可用下限：以“欠载稳态收敛 + 无一票否决项”为硬门，`skip/dec` 分级为软据，
 取 HDMI 与文件源两轮下限的较大者，记为候选低延时档。
2. 若候选档仍远高于个位数毫秒，说明抖动源不在播出缓冲，回查 MTL 调度与解码
 p99 尾部，不再继续压 delay。
3. 落盘：候选档写入验证报告与 `thresholds.json` 收敛值；默认 `80ms` 是否同步下调、
 是否增设低延时档配置，单独走评审，不在扫参轮内顺手改。
4. 归档：三件套按日期归档到 `docs/archive/perf/`，并在 `rk3588-rx-verification-gap-analysis.md`
 的 delay 曲线缺口项下注销本轮结论。
5. 预算表不因扫参改 verdict：playout 归零后的典型/最坏值仍以 T1/T2 实测为准，
 本轮只提供低延时档参数，不改变“当前生产配置下 <10ms 不成立”的结论。

## 4 关闭条件

两轮扫参（HDMI + 文件源）见证齐全、候选档有明确数值与人眼依据、
缺口项已注销，视为 T4 关闭；线程模型维持现状的决策不受本轮影响。
