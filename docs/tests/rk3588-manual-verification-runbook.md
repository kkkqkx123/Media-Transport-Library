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
# 2) 二进制若有 C 改动：在 devs 交叉重编后再推（板上不编译，见 §5；本地禁构建，见 AGENTS.md）
scp app/rk3588_playout_rx/build/file_playout_rx board:~/code/mtl/bin/
# DVS 节点包（devs 产物经本地中转，devs 不能直连 board）：
scp devs:/data/kkkqkx/mtl/app/dvs_media/build-aarch64/dvs_media /tmp/ && scp /tmp/dvs_media board:~/code/mtl/bin/
scp devs:/data/kkkqkx/mtl/app/dvs_agent/build-aarch64/dvs_agent /tmp/ && scp /tmp/dvs_agent board:~/code/mtl/bin/
scp debug/tests/dvs_node_smoke.sh debug/tests/dvs_agent_api_smoke.sh debug/tests/dvs_ipc_probe.py board:~/code/mtl/src/debug/tests/
scp -r app/dvs_agent/web/src board:~/code/mtl/src/dvs-agent-web
# 3) 板上拷可执行副本进 bin/ 并赋权
ssh board "cp ~/code/mtl/src/debug/tests/board_env.sh ~/code/mtl/src/debug/tests/dvs_node_smoke.sh ~/code/mtl/src/debug/tests/dvs_agent_api_smoke.sh ~/code/mtl/src/debug/tests/dvs_ipc_probe.py ~/code/mtl/src/playout-tests/run_tx_rx_e2e.sh ~/code/mtl/bin/ && chmod +x ~/code/mtl/bin/board_env.sh ~/code/mtl/bin/dvs_node_smoke.sh ~/code/mtl/bin/dvs_agent_api_smoke.sh ~/code/mtl/bin/dvs_ipc_probe.py ~/code/mtl/bin/dvs_media ~/code/mtl/bin/dvs_agent ~/code/mtl/bin/run_tx_rx_e2e.sh"
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

- C 改动只在 devs 交叉重编（`app/rk3588_live_tx`、`app/rk3588_playout_rx`、`app/dvs_media`、`app/dvs_agent` 各自 `ninja -C build-aarch64` 增量即可；devs 上 PATH 需含工具链 `bin` 与 buildroot `host/bin`，否则 meson 再生钩找不到 `ar`/python），推 `bin/` 后 `md5` 对齐；板上只 `gcc` 直编 `mpp_*_latency/drm_smoke/hdmi_passthrough/alsa_tone/v4l2_cap_latency` 类探针。本地禁构建，见 AGENTS.md。
- `run --` 正常/中断都会 `restore`；若桌面没回来：`systemctl start lightdm`，再 `pgrep Xorg` 确认。
- `/tmp/board_env.state` 是准备记录（重启即失，默认即桌面态）；`release` 不动桌面，可随时单独用。
- DVS 清场专用：`dvs_media` 忽略 SIGTERM，必须按 pid `kill -9`；再删测试 socket（`/tmp/dvs-media.sock`）与临时测试 IP；最后 `board_env.sh status` 确认 DRM/UDP/声卡释放。MtlManager 若因 XDP 挂载失败退出，重拉即可（`nohup /usr/local/bin/MtlManager >/tmp/mtlmanager2.log 2>&1 &`），空载常驻。

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
| `POST /media/commit` 500 `layout apply failed` | 流参数缺项 | 查引擎日志 `invalid rect`（补 `x/y/w/h`）/`require exactly one main=true`（补 `main:true`）/`not whitelisted`（先配白名单）；IPC 用驼峰 `udpPort`，静态 JSON 用下划线 `udp_port`，别混 |
| `dvs_media` 杀不掉（pgrep 还在） | 忽略 SIGTERM（DPDK 阻塞） | 按 pid `kill -9`，不是 skill 问题 |
| MTL `native_af_xdp` 初始化失败 | 硬件驱动约束，非问题 | 本板用 `kernel:` 后端（与 `run_tx_rx_e2e.sh` 默认一致）；enP3p49s0 无 IP 时先临时配测后删 |
| MtlManager 突然没了 | XDP 挂载失败会拖死它 | 重拉即常驻（见 §5）；改走 `kernel:` 后不再触发 |

通用顺序仍是：参数对齐 → 单路冒烟 → 再怀疑网络或争用（见 README §5）。

## 7 取证与报告

每轮一页：拓扑、源类型、参数摘要、五级帧数、时延分位、音频状态、硬件负载、结论与定位
（失败必须指明发送/网络/接收三段中的一段）。取证物即 `OUT_DIR` 当轮文件＋`perf/` 输出，
归档到 `docs/archive/perf/`（按日期命名）。数值对照上轮同场景 validation-report，
阈值收敛统一改 `tests/perf/common/thresholds.json`，不在脚本里硬编码。
DVS 节点包的人工验证见 §8，结论记 `docs/archive/app/`（按日期命名）。

## 8 DVS 节点包人工验证（V3/V4/V5/V7，需人眼与浏览器）

机器能跑的部分（V0–V2、V6 状态机、单测）由 `dvs_node_smoke.sh` 与
`dvs_agent_api_smoke.sh` 覆盖，见 `dvs-node-validation-report.md`。
本节只写机器替不了的三件事：浏览器点一遍、显示器看一眼、长稳守一次。
前置：§2 门禁全绿；`dvs_media`、`dvs_agent`、`dvs_*_smoke.sh` 已按 §1 推板且 `md5` 对齐；
本板一律用 `kernel:` 后端（AF_XDP 属硬件驱动约束，见 §6）。

### 8.1 V3 浏览器走查（节点地址 `http://<板IP>:<端口>/`，默认 `admin/admin123` 首登改密）

逐区操作，每区看配置状态行（`pending/applying/applied/failed`，失败必带可读原因）：

| 区 | 做 | 看到什么算过 |
|---|---|---|
| 节点角色 | 切输入盒→撤销 | 待重启标记出现再消失 |
| 输入口 | 填采集口 + 编码 + 目标后保存 | 发送状态标签与 `GET /media/tx` 一致；只存采集口则不启动发送 |
| 输出设置 | 超限几何保存 | 明确失败（能力上限），不静默通过 |
| 网络与注册 | 静态坏 IP 保存 | 明确失败；切 DHCP/静态只落意图，不切系统网（隔离确认才真切） |
| SIP | 开关各一次 | 连通性状态变化，媒体不受影响 |
| 本机信息 | 浏览 | 型号/角色/IP/注册态/当前播放/CPU 内存温度齐全 |
| 维护操作 | 只看不点 | `reboot/factory-reset/upgrade` 验证中一律不执行 |

### 8.2 V4 单板回环出画（发流端 + 引擎 + 看屏）

三终端同板执行（`BIN=~/code/mtl/bin`，`IP=192.168.80.202`，`UDP=15000`）：

```bash
# 终端 A：受控空闲引擎（无 pip_streams，只建 MTL + 握 IPC）
./dvs_media --config ~/code/mtl/perf/dvs_media_idle.json --ipc-sock /tmp/dvs-media.sock
# 终端 B：代理（连上一步 socket）
./dvs_agent --db /tmp/manual.db --port 18082 --media-sock /tmp/dvs-media.sock --web-root ~/code/mtl/src/dvs-agent-web
# 终端 C：HDMI 发流（与 run_tx_rx_e2e.sh 同参数）
./live_ingest 300 --port kernel:enP2p33s0 --lip $IP --dip $IP --udp $UDP --pacing tsc_narrow
# 终端 C 另起：下发一路（main:true + 几何缺一不可，见 §6）
curl -X POST http://127.0.0.1:18082/api/v1/node/media/prepare -H 'Content-Type: application/json' \
  -d '{"operationId":"manual-v4-1","revision":1,"streams":[{"main":true,"sip":"192.168.80.202","srcIp":"192.168.80.202","udpPort":15000,"width":1920,"height":1080,"fps":30,"x":0,"y":0,"w":1920,"h":1080}]}'
# → 202 preparing；3 秒后查 status 为 prepared 再 commit：
curl -X POST http://127.0.0.1:18082/api/v1/node/media/commit -H 'Content-Type: application/json' \
  -d '{"operationId":"manual-v4-1","revision":1}'
# → committed {revision 1}；再查 status：rev 1、lanes 1
```

看屏（显示器接板载 HDMI 输出口）：连续看 60 秒以上，逐项回答并记入
`~/code/mtl/perf/dvs_manual_witness_$(date +%F).log`：

1. 有连续画面且内容随源变化（非冻结帧）？y/n
2. 无黑场/花屏/撕裂/横向分块？y/n
3. 停发流（`Ctrl-C` 发流端）后画面冻结不崩，恢复发流后自愈？y/n

三项全 y 记 `WITNESSED_PASS`，任一 n 记 `WITNESSED_FAIL` 并描述现象；
非交互只跑流标 `UNWITNESSED`，不伪造。

### 8.3 V5 输入盒发送（角色切换 + 发包验证）

```bash
# 切输入角色（pendingRole 置位，重启生效；撤销可回退）
curl -X POST http://127.0.0.1:18082/api/v1/node/config/role -H 'Content-Type: application/json' \
  -d '{"desiredValue":{"role":"input"},"operationId":"manual-role-1"}'
# 下发采集发送三元组（编码 + 目标同时给才启动，只给采集口不发流）
curl -X POST http://127.0.0.1:18082/api/v1/node/config/input -H 'Content-Type: application/json' \
  -d '{"desiredValue":{"port":"HDMI1","encode":{"codec":"h264","width":1920,"height":1080,"fps":30,"bitrateKbps":4000},"target":{"sip":"192.168.80.202","udpPort":15010}},"operationId":"manual-input-1"}'
# 查 tx 状态机：idle → configured → running
curl -s http://127.0.0.1:18082/api/v1/node/media/tx
```

`state: running` 后，在目标端抓包或另起一路接收（`file_playout_rx … --udp 15010`）
确认有包；超限参数（分辨率/码率/端口）必须进 `failed`。非法角色值必须被拒。
验证完切回 `output` 并撤销 pending，还原输出盒身份。

### 8.4 V7 长稳（30 分钟起步）

保持 §8.2 回环跑满 30 分钟，每 5 分钟记一次 `query_state`（`rxSessions/decoding/layers`）
与整机负载；判定只有两条：产出比不随时间衰减；资源无单调泄漏。不做主观画质评分。

### 8.5 清场（每次人工验证后必做）

```bash
pkill -x dvs_agent; pkill -x dvs_media; sleep 2; pgrep -x dvs_media && kill -9 $(pgrep -x dvs_media)
rm -f /tmp/dvs-media.sock; ./board_env.sh status   # 回到全绿（含 MtlManager 运行中）
```

桌面没回来按 §5 处理；测试 IP 若配过必须删（`ip addr del`）。
