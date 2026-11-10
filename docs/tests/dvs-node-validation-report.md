# DVS 节点包验证报告（开发板实测）

日期：2026-11-10（UTC）。设计口径见 `dvs-node-deployment-verification-design.md`。破坏性运维（`reboot`/`factory-reset`/`upgrade`）一律未执行。板上调试位置统一为 `~/code/mtl`（`bin/` 运行态，`src/` 只读镜像，`perf/` 数据态），见 `docs/dev/board-mtl-sync.md`。

## 1 编译（devs，aarch64）

本地禁止任何 x86 构建（见 `AGENTS.md` 编译硬性规定），`app/dvs_agent/build`、`app/dvs_platform/build` 已删除；一切编译在 devs（`/data/kkkqkx/mtl`）完成。

- devs 先 `reset --hard` 到与本地同提交（`e5c236ed update url routing`），再增量同步 9 个文件（3 脚本 + 设计 + 报告 + 清单 + README + `CLAUDE.md` + `MTL_SYNC_GUIDE.md`），无全量 scp。
- devs 构建曾被陈旧 `build.ninja` 再生钩阻塞：构建目录绑定的 buildroot meson 用系统 python3.6 跑不起来。修复 PATH（工具链 `bin` + buildroot `host/bin` 在前，提供可用 meson 1.3.1 与 python3.11 及 `aarch64-none-linux-gnu-ar`）后一次通过：
  - `ninja -C app/dvs_agent/build-aarch64` → exit 0
  - `ninja -C app/dvs_media/build-aarch64` → exit 0
- 产物均为 `ARM aarch64`，`dvs_agent` 与 `dvs_media` 各一，已推板 `~/code/mtl/bin/`。

## 2 板端门禁与版本（V0）

- `board_env.sh status` 全绿：card0 空闲、HDMI-A-1 已连接、video0 空闲且有时序锁存、ALSA 就绪、无 PulseAudio、网卡就绪、UDP 空闲、MtlManager 运行中。
- `./dvs_media --version` 与 `./dvs_agent --version` 均为 `bundle 1 ipc 1`。

## 3 引擎 IPC（V1，真机能力）

- `hello`：`ipcVersion 1/bundleVersion 1/state idle/revision 0`。
- `capability`（实测值）：输入 `HDMI1`（`/dev/video0`，有信号，3840x2160@60）；输出 `HDMI-A-1`（1920x1080@75，31 modes）；`maxDecodeSessions 4`；`maxLayers 4`；TX 上限 h264/h265、3840x2160@60、20000kbps；`mtl version 26.9.0.DEV/backend kernel`；PTP synced。
- `query_state`：`state/revision/lanes/audio/display/rxSessions/decoding/layers/ptp/tx` 全字段齐备，空载 `lanes []`。

## 4 代理 Web 后端（V2+V3）

- 无引擎时两脚本 `OVERALL: PASS`（媒体面为离线诚实失败，非空串）。
- 有引擎时版本门：`match true/engineLink online/failStreak 0`。
- `dvs_node_smoke.sh OVERALL: PASS`（双版本同包、IPC 三动词、4 组 HTTP 包络、版本门字段、前端 `/` 与 `node-admin.html` 200）。
- `dvs_agent_api_smoke.sh OVERALL: PASS`（26 项：登录对错分流、六 scope 查询、未知 scope 拒、角色/输出/网络三非法写入进 `failed`、媒体 6 面在线真值、审计/本地投影、版本门）。

## 5 媒体单测（板上 8/8）

`test_state_machine/test_ipc/test_session_admit/test_tx_session/test_tx_port_map/test_stream_validate/test_audio_owner/test_capability_logic` 全过（21 行 PASS/ok，0 failure），日志在板 `perf/dvs_media_unittests.log`。

## 6 受控热更新（V6，经代理）

- `POST /media/prepare` → 202 preparing；轮询 `query_state` → prepared、lanes 1。
- 反例（均诚实失败、状态不伪造）：缺渲染几何报 `invalid rect (0,0 0x0)`；缺 `main:true` 报 `require exactly one main=true (got 0)`；abort 后重做。
- `POST /media/commit` → `committed {frameIndex 0, late 0, revision 1}`；提交态 `idle/rev 1/lanes 1/rx 1/layers 1/ptp synced`（无源故 `ready false/decoding false`，诚实零值）。
- 已清场：引擎与代理测试进程已杀、测试 IP（enP3p49s0 上 192.168.81.213/24）已删、陈旧 socket 已删；MtlManager 已重拉并空载常驻 30 秒以上。

## 7 板端环境结论与约束

- AF_XDP 属硬件驱动约束，不记为问题：本板当前驱动组合不支持 XDP 程序加载（`xdp-dispatcher.o` 报 EINVAL 且缺 `xsk_def_xdp_prog.o`），本轮全部走 `kernel:` 后端（与 `run_tx_rx_e2e.sh` 默认一致）。另注意 MtlManager 会在 XDP 挂载失败时退出，改走 `kernel:` 后重拉即可常驻。
- MTL 双口初始化要求两 NIC 都有 IP：enP3p49s0 无载波无 IP 时 `mt_socket_get_if_ip fail`；临时配测 IP 验证通过后已删除。
- `dvs_media` 忽略 SIGTERM（DPDK 阻塞），清场必须 `kill -9 <pid>`；`MtlManager` 由 `board_env`/`run_tx_rx_e2e.sh` 按需拉起。

## 8 待验（需发流端 + 人眼）

实际出画（V4 独立静态多路、V5 输入盒发送、V7 长稳）需对端发流与显示见证，本轮未做；按 `rk3588-manual-verification-runbook.md` 一条龙执行，结论记 `WITNESSED_PASS/FAIL`。
