# RK3588 手动验证操作手册（runbook）

> 定位：拿板后从零跑出第一次 `OVERALL: PASS` 的唯一动手入口。设计口径见本目录各
> `*-verification-design.md`，脚本与板端位置对照见 `manual-scripts-inventory.md`，
> 同步规范见 `../dev/board-mtl-sync.md`。本文只写“我敲什么、看到什么算过、
> 报错了找谁”，不重复设计判定。
>
> 阅读顺序：本文 → 要跑场景对应的 design → 上轮同场景 validation-report（对照数值）。

## 1 同步（仓库侧执行，5 步）

约定重申：仓库永远是源，板上永不改 `src/`，`bin/` 中 `.sh` 必须是 `src/` 的可执行副本。

```bash
# 1) 推脚本（按需取子集；board_env.sh 必推，它是所有 e2e 的门禁）
scp debug/tests/board_env.sh board:~/code/mtl/src/debug/tests/
scp app/rk3588_playout_rx/tests/run_tx_rx_e2e.sh board:~/code/mtl/src/playout-tests/
# 2) 二进制若有 C 改动：本地交叉重编后再推（板上不编译，见 §5）
scp app/rk3588_playout_rx/build/file_playout_rx board:~/code/mtl/bin/
# 3) 板上拷可执行副本进 bin/ 并赋权
ssh board "cp ~/code/mtl/src/debug/tests/board_env.sh ~/code/mtl/src/playout-tests/run_tx_rx_e2e.sh ~/code/mtl/bin/ && chmod +x ~/code/mtl/bin/board_env.sh ~/code/mtl/bin/run_tx_rx_e2e.sh"
# 4) 抽查 md5，全 SAME 方为对齐
ssh board "md5sum ~/code/mtl/src/debug/tests/board_env.sh ~/code/mtl/bin/board_env.sh"
md5sum debug/tests/board_env.sh
```

## 2 门禁（板上执行，先看环境再跑流）

```bash
cd ~/code/mtl/bin && ./board_env.sh status
```

- `环境就绪`：直接进 §3。
- 每个 `BLOCKED` 行就是动作，不用猜：
  - `card0 被占用：*(Xorg)` → 跑 §3 的一条龙（自动停桌面），或 `board_env.sh prepare display --yes`。
  - `HDMI 输入无信号` → 先按 `../issue/RK3588_HDMIIN_SOURCE_INSTABILITY.md §3` 预检源；无信号时只允许跑文件源场景（T1），不许把空采记为链路失败。
  - `PulseAudio 运行中` → 同桌面处理（`prepare audio/all`）。
  - `UDP 端口被占用` / `video0 被占用` → `board_env.sh release` 急救（只清应用级残留，不动桌面）。
  - `MtlManager 未运行` → 门禁所在脚本会自动拉起；手动拉起只为排查。

## 3 标准流程（一条龙，含中断恢复）

`run --` 的语义：准备→跑→恢复，`Ctrl-C` 也会先恢复桌面再退出。结束后 `status` 应回到桌面态。

```bash
cd ~/code/mtl/bin
./board_env.sh run -- ./run_tx_rx_e2e.sh 60        # 单路基线
./board_env.sh run -- ./run_multinode_composite.sh T2 40  # 双路合成（T0/T1/T3 同理可单独跑）
./board_env.sh run -- ./display_compare.sh         # A/B 对照：先直通后链路，按提示看屏
./board_env.sh run -- ./manual_check_visual.sh T2 120    # 看屏见证：逐项 y/n，结论进见证文件
```

期望输出：机器部分以 `OVERALL: PASS`（单路）或各 `PASS: T*` 行收尾，末尾汇总行引用的是末值窗口
（首尾窗口含启停瞬态，不看）；显示器上有连续画面；`run` 结束后桌面回来。
`OUT_DIR`（默认 `/tmp/multinode_composite`）内当轮发送日志、接收日志、JSON 同目录存放。

## 4 看屏确认（日志全绿≠人眼通过）

`manual_check_visual.sh` 会逐项问，以下三件事只有人眼能定（曾有日志全绿但屏幕横向 3 分块的前科）：

1. 像素正确性：路数、窗口几何与配置一致，每路内容随各自源变化且可区分。
2. 持续稳定性：至少连续观察 60 秒，无黑场/花屏/撕裂/横向分块。
3. 音频可听性（T1）：显示器扬声器出声连续，无发声设备答 `s` 跳过并注明。

结论记入 `manual_witness_*.log`（`WITNESSED_PASS/FAIL`），非交互终端只跑流并标 `UNWITNESSED`，不伪造结论。

## 5 二进制与环境恢复

- C 改动只在交叉编译服务器重编（`app/rk3588_live_tx`、`app/rk3588_playout_rx` 各自 `ninja -C build` 增量即可），推 `bin/` 后 `md5` 对齐；板上只 `gcc` 直编 `mpp_*_latency/drm_smoke/hdmi_passthrough/alsa_tone/v4l2_cap_latency` 类探针。
- `run --` 正常/中断都会 `restore`；若桌面没回来：`systemctl start lightdm`，再 `pgrep Xorg` 确认。
- `/tmp/board_env.state` 是准备记录（重启即失，默认即桌面态）；`release` 不动桌面，可随时单独用。

## 6 故障速查（症状→动作，不展开）

| 症状 | 先查 | 动作 |
|---|---|---|
| `MTL start failed` | 残留进程争用端口 | `release` 后重跑（`run --` 自带清理） |
| `TX build ret -283` / `trs -142` | 同上（应用无帧可发/发送环无包是结果） | 同上，先清残留再谈链路 |
| `ALSA open failed` / `mtl_rx_audio` 无建连 | 声卡被占或设备名不对 | `status` 定责 → `prepare audio/all` |
| `render` 远小于 `dec` | 预期内（按时钟取最新帧主动跳帧） | 只看趋势与零错误，不做相等断言 |
| `composite_frames=0` | lane 参数错配（端口/宽高帧率） | 先对齐饰演参数，再查码流上限与播放延迟 |
| 显示提交失败/黑屏但日志正常 | Xorg 占 master | 走 `run --`（自动停桌面），勿 `fuser -k card0` |
| HDMI 时序跳变中途截断 | 源抖动 | 按 issue 文档 §3 预检源，抖动窗口内只跑 T1 |

通用顺序仍是：参数对齐 → 单路冒烟 → 再怀疑网络或争用（见 README §5）。

## 7 取证与报告

每轮一页：拓扑、源类型、参数摘要、五级帧数、时延分位、音频状态、硬件负载、结论与定位
（失败必须指明发送/网络/接收三段中的一段）。取证物即 `OUT_DIR` 当轮文件＋`perf/` 输出，
归档到 `docs/archive/perf/`（按日期命名）。数值对照上轮同场景 validation-report，
阈值收敛统一改 `tests/perf/common/thresholds.json`，不在脚本里硬编码。
