# dvs_agent 功能分析

> 分析对象：`app/dvs_agent/`（节点代理，C++17）。编写时间：2026-11-08，基于 `feat/rk3588-apps` 分支。

## 1. 定位

运行在每个节点（RK3588 盒子）上的**纯控制面代理**：对外提供本地 HTTP API 与静态管理前端（node-device-admin），对内经 Unix socket IPC 驱动本机 `dvs_media` 引擎，向上向 `dvs_platform` 网关注册/心跳并接收下行指令。依赖仅 json-c + 内置 SQLite（`dvs_common` in-tree 编译），无其他运行时依赖。

与 `dvs_media` 同节点包发布（`deploy/dvs-node/`，版本契约 `dvs_common/include/dvs_node_bundle.h`）：`--version` 打印包版本；两个二进制必须同包，任一不符即拒绝拉起，`GET /api/v1/node/version` 呈现配对状态。

```bash
cd app/dvs_agent && meson setup build && ninja -C build && meson test -C build
```

## 2. 模块结构

| 文件 | 职责 |
|---|---|
| `main.cpp` | 入口：装配各模块、HTTP 路由（API + 静态前端）、六 scope 配置状态机、下行指令分发（layout.prepare / config.update / upgrade.task）、tx_start 转发 |
| `node_store.cpp/.h` | SQLite 节点状态存储：node 文档、**六 scope 配置状态机**（`SCOPES[6]`：desired/applied/failed/retry、revision）、管理密码（PBKDF2-HMAC-SHA256 + 随机盐，首次登录强制改密）、`pendingRole` 延迟角色切换（重启时生效） |
| `registration.cpp/.h` | 平台注册与心跳：优先 WS 长连接（同端口 `/api/v1/channel/ws`），失败回退短连接；`DVS_TRANSPORT_MODE=short/auto/ws` 可选；空 platform URL = 本地自治；下行指令经 `DownlinkHandler` 回调，回执走 `uplink_*` |
| `media_ipc.cpp/.h` | `dvs_media` IPC 客户端：持久连接 + 按需重连、requestId 回显、结构化错误（`MEDIA_ENGINE_UNREACHABLE/TIMEOUT`）、事件环形缓存（64 条固定 ring，规避旧 libstdc++ 的 deque）、`wait_prepare_result` 阻塞等待异步 prepare 结果、`parse_hello` 版本门（纯函数可单测）；**`translate_platform_layout`** 把平台子布局（flat windows + endpoint 表 + 音频 owner/音量）翻译成引擎 `streams` prepare payload（z 升序分配 lane，整单拒绝式校验） |
| `tx_config.cpp/.h` | input scope TX 参数模型：解析/默认值/范围校验；上限取冻结 SoC 规格（3840×2160@60、20Mbps），有 `capabilities.tx` 上报则逐字段收窄（reported wins） |
| `network_apply.cpp/.h` | 真实网络配置落地（systemd-networkd 单风格）：解析 → 渲染 `.network` 文件（纯函数）→ 快照/写入/reload/连通性检查/失败回滚；**执行门**：`DVS_ALLOW_NETWORK_APPLY=1` AND confirm AND opid 三重确认，接口名白名单防注入 |
| `maintenance.cpp/.h` | 维护操作：reboot（落地 pendingRole）、factory reset（保留网络与注册信息）、固件升级（SHA-256 校验、A/B staging、失败保留旧版本） |
| `web/` | node-device-admin 静态前端（页面见 `web/src/pages/`，交付说明 `web/src/开发交付说明.md`），登录限流每来源 5 次/60 秒 |
| `tests/test_basic.cpp` | 基础自测（`meson test`） |

## 3. 关键机制

### 3.1 注册/心跳与 WS 通道

- 首包 `hello` 携带 token，经共享契约校验；后续心跳为 WS 上行消息（默认 3s 周期，含 CPU/内存/负载/uptime 容量自检，读 `/proc` 零依赖）。
- 下行三类指令：`layout.prepare`（布局变更）、`config.update`（配置下发）、`upgrade.task`（升级任务）。
- 回执上行：`uplink_barrier_ack`（含帧锚定 commit 报告 frame_index/applied_at/late，透传到平台屏障）、`uplink_config_ack`、`uplink_upgrade_progress`。WS 不可用时回退短连接。
- 传输失败永不抛异常、不干扰媒体面。

### 3.2 六 scope 配置状态机

`NodeStore` 以统一状态机管理六类配置 scope：`set_desired`（登记期望值 + opid）→ `mark_applied`（引擎确认 revision）→ `mark_failed`（记录 code/reason）→ `retry`。所有 scope 状态可经 `all_states` / `config_revision` 查询，前端与平台据此核对收敛情况。

### 3.3 平台布局 → 引擎 payload 翻译

`translate_platform_layout` 是代理的核心翻译层：输入平台下发的子布局（窗口 windowId/sig/dst/src/z/muted/partial、endpoint 地址端口表、window-id 音频 owner、场景音量），输出引擎 `streams` prepare payload。规则：

- lane 按 z 升序分配（并列按数组序），逐流 fps 与 audioUdpPort 填入；
- 音频 owner 的 window-id 解析为本节点 `lane<N>`（本节点无此 lane 则省略）；
- 信号不在 endpoint 表、endpoint 缺 fps、超出 lane 上限 → **整单拒绝**，不做部分应用。

### 3.4 诚实状态与安全门

- IPC 客户端以 fail_streak / last_ok_ms 记录引擎链路健康，query 不做额外探测；
- 网络配置必须三重确认才真实执行，且自带快照回滚；
- 升级先 SHA-256 校验再 A/B staging，失败保留旧版本；
- 密码 PBKDF2 存储 + 首次改密强制 + 登录限流。

## 4. 与其他模块的边界

- **dvs_media**：代理只做转发与翻译，媒体执行细节（准入、合成、状态机）全在引擎侧；`hello` 握手核对包版本后才开始转发。
- **dvs_platform**：代理是平台的执行端点；prepare/commit 屏障、升级编排的决策全在平台，代理只回执与执行。
- **dvs_common**：复用 HTTP 服务/客户端、SHA-256、SQLite 封装与 bundle 版本头。
