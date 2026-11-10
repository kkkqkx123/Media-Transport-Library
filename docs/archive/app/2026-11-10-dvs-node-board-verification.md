# 2026-11-10 DVS 节点包开发板验证（步骤与结果）

归档口径：`docs/tests/dvs-node-deployment-verification-design.md`（设计）、
`docs/tests/dvs-node-validation-report.md`（结论）。本文件只归档本轮在开发板上的
执行步骤与实测输出。破坏性运维（`reboot`/`factory-reset`/`upgrade`）一律未执行。

## 0 环境与产物来源

- 开发板：RK3588，内核 5.10.226，`board_env.sh status` 全绿（card0 空闲、HDMI-A-1 已连接、video0 有时序锁存、ALSA 就绪、网卡 enP2p33s0 就绪、MtlManager 运行中）。
- 分支：`feat/rk3588-apps`（`e5c236ed update url routing`），devs 与本地同提交。
- 编译（devs，aarch64）：`ninja -C app/dvs_agent/build-aarch64` 与 `ninja -C app/dvs_media/build-aarch64` 均为 exit 0（PATH 需含工具链 `bin` 与 buildroot `host/bin`，否则 meson 再生钩找不到 `ar`/python）。产物推板 `~/code/mtl/bin/`。
- 调试脚本（`debug/tests/` → 板 `src/debug/tests/`，可执行副本进 `bin/`，md5 三端一致）：`dvs_ipc_probe.py`、`dvs_node_smoke.sh`、`dvs_agent_api_smoke.sh`；静态前端 `app/dvs_agent/web/src` → 板 `src/dvs-agent-web/`。

## 1 步骤与结果

### V0 打包与版本

```bash
./dvs_media --version   # dvs-media bundle 1 ipc 1
./dvs_agent --version   # dvs-agent bundle 1 ipc 1
```

包版本与 IPC 代际一致，准入通过。

### V1 引擎 IPC（`python3 dvs_ipc_probe.py --sock /tmp/dvs-media.sock --type <verb>`）

- `hello` → `ok:true`，`{ipcVersion 1, bundleVersion 1, state idle, revision 0}`。
- `capability` → 输入 `HDMI1`（`/dev/video0`，有信号，3840x2160@60）；输出 `HDMI-A-1`（1920x1080@75，31 modes）；`maxDecodeSessions 4`；`maxLayers 4`；`mtl version 26.9.0.DEV/backend kernel`；PTP synced。
- `query_state` → `state/revision/lanes/audio/display/rxSessions/decoding/layers/ptp/tx` 全字段，空载 `lanes []`。

### V2+V3 代理与前端

- 无引擎：`dvs_node_smoke.sh` 与 `dvs_agent_api_smoke.sh`（26 项）均为 `OVERALL: PASS`，媒体面为离线诚实失败。
- 有引擎（`--media-sock`）：`GET /api/v1/node/version` → `match true/engineLink online/failStreak 0`；媒体 6 面全部在线真值；静态前端 `/` 与 `node-admin.html` 200。

### 单测（板上 8/8）

`test_state_machine/test_ipc/test_session_admit/test_tx_session/test_tx_port_map/test_stream_validate/test_audio_owner/test_capability_logic` 全过（21 行 PASS/ok，0 failure）。

### V6 受控热更新（经代理 HTTP）

```bash
POST /media/prepare {"operationId":"board-v6-3","revision":1,"streams":[{"main":true,"sip":"127.0.0.1","srcIp":"127.0.0.1","udpPort":15000,"width":1920,"height":1080,"fps":30,"x":0,"y":0,"w":1920,"h":1080}]}
# → 202 preparing；轮询 query_state → prepared、lanes 1
POST /media/commit {"operationId":"board-v6-3","revision":1}
# → committed {frameIndex 0, late 0, revision 1}；提交态 idle/rev 1/lanes 1/rx 1/layers 1/ptp synced
```

反例（均诚实失败）：缺渲染几何报 `invalid rect (0,0 0x0)`；缺 `main:true` 报 `require exactly one main=true`；`abort` 回 idle 正常。

## 2 硬件驱动约束（不记为问题）

- AF_XDP 可用性由网卡驱动/XDP 程序支持决定：本板当前驱动组合不支持（dispatcher 加载失败且缺 `xsk_def_xdp_prog.o`），统一走 `kernel:` 后端。
- XDP 挂载失败会拖死 MtlManager，改走 `kernel:` 后重拉即常驻。
- 双口初始化要求两 NIC 都有 IP（enP3p49s0 临时配测后已删）。
- `dvs_media` 忽略 SIGTERM，清场用 `kill -9 <pid>`。

## 3 收尾与待验

已清场：测试进程已杀、测试 IP 已删、陈旧 socket 已删，`board_env.sh status` 恢复全绿。
待验（需发流端 + 人眼）：V4 独立多路出画、V5 输入盒发送、V7 长稳，按 runbook 一条龙执行。
