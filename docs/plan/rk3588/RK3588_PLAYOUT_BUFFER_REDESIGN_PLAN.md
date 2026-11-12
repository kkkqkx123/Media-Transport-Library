# 固定等待 vs 真正播出缓冲：问题分析与改造方案

## 1 现状判定

当前 `playout_delay` 不是通常意义的抖动缓冲，而是**固定偏移播出**。
到期条件是 `now >= pts + delay`，其中 `pts` 是源时间线
（RTP 90kHz 经 `media_clk` 重建，首帧到达锚定；TX 默认不用 TAI）。
因此锚定之后到达的网络抖动落在 `delay` 窗口内时确实被吸收，
呈现跟源时间线不跟到达时刻——`80ms` 的生产稳态连续性就来自这里，
它不是毫无意义，只是按固定价格（常驻 +80ms E2E）购买窗口内平滑。

## 2 三个修不掉的缺陷（调参无解，只能改 policy）

1. 锚定一次性锁定：首帧到达时刻（含当轮 transit 与启动瞬态）永久 baked
 进整条时间线，无重锚定。回环下偏置是常数所以看不出来，
 真实网络下首帧若撞上抖动尖峰，整轮带着偏置走。
2. 无漂移跟踪：发送端 90kHz 与本地时钟长期漂移时，时间线与 `now`
 越差越远，表现为系统性欠载或延迟缓慢增长加周期跳帧。
 现在 `skew_reject` 只报错作废，不做纠正。
3. 无水位管理：解码服务约 3~4.5ms 是刚性前置成本，
 `delay` 小于它时缓冲 occupancy 恒 0/1，16 深队列退化成直通。
 `delay` 只决定截断点，不决定平滑度。

## 3 T4 数据对设计的输入（2026-11-12 实测）

- 60fps：skip 与 underrun 跷跷板，中间档双高，可选工作点只有收敛档与直通档。
- 30fps：skip 与 delay 基本无关（恒 ~8%），只有 under 发散，机理待查。
- 两轮 HDMI 复现度 1~3%，曲线稳定。
- 详见 `docs/archive/tx-rx/2026-11-12-playout-delay-sweep.md`。
 人眼结论：0ms 与 80ms 相比视觉不差，可选直通档，默认已改为 0。

## 4 改造方案（分阶段，只动到期 policy，不动线程划分）

- P0 启动水位锚定：首帧不等，攒到目标水位（如 2 帧）再定锚点，
 消启动瞬态偏置。动 `render_thread` 启动段与 `av_sync`。
- P1 水位伺服重锚定：跟踪 `playout_buffer` occupancy 的 EMA，
 持续偏低/偏高时做一次性时间线前移/后移（一次跳帧校正），
 替代现在的每帧 skip/underrun 慢性失血。
- P2 时钟恢复：用 SEI `capture_tai` 与本地锚定线的长期漂移率估计 skew，
 微调 `anchor/frame_period`；到期判断仍只用 `pts/media_clk`，
 文件源与 HDMI 源通用，不依赖 SEI。
- P3 早晚帧策略分离：网络晚（`pts` 间隙）重显上一帧保节奏，
 解码晚（`push` 已过期）维持当前 skip 策略。

顺序：人眼结论已出（选直通档），启动水位与水位伺服已按本方案实施，
用同一套 sweep 脚本前后对照（skip/under 曲线下移为验收标准），有效果再议 P2/P3。
约束：compositor 多 lane 保持 main lane 锚；预算线程模型维持现状的决策不受影响。

## 5 与其他文档关系

| 文档 | 关系 |
|---|---|
| `RK3588_1080P_SUB10MS_E2E_BUDGET_AND_THREAD_MODEL.md` | 判定依据；本改造不改变其 verdict |
| `RK3588_PLAYOUT_DELAY_SWEEP_FOLLOWUP.md` | T4 收尾；本方案消费其候选档与数据 |
| `docs/archive/tx-rx/2026-11-12-playout-delay-sweep.md` | 本方案的数据输入 |
| `docs/tests/rk3588-manual-verification-runbook.md` | 验证入口；会话级桌面模式见该文档 §3 |
