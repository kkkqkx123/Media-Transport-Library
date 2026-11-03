# DVS 前端正式化：细化实施计划

基于 `prototype-vs-formal.md`。目标：把 `app/dvs_agent/web/src/packs/node-admin/node-admin.html` 与 `_shared` 从 localStorage 原型切换为真实 dvs-agent HTTP 数据源，并完成品牌/文案/文档收尾。

## 0. 约束与约定

- 后端真实接口以 `app/dvs_agent/main.cpp` 路由为准，**不是**原型文档中假设的 `/maintenance/...` 或 `/media/{state,stats}` 路径。
  - 配置：`/api/v1/node/config/:scope`、`/retry`
  - 媒体：`/api/v1/node/media/status`
  - 维护：`/api/v1/node/reboot`、`/api/v1/node/factory-reset`、`/api/v1/node/upgrade`
  - 其余：`/api/v1/node/info`、`/capability`、`/role`、`/password`、`/api/v1/audit-events`
- 登录响应：`data.requirement.passwordChangeRequired`（嵌套）。
- 真实后端状态取值：`idle` / `pending_apply` / `applied` / `failed`（无 `applying`，前端自行在提交后短暂显示"应用中"或直接轮询）。
- 适配层唯一：`_shared/real-api.js`；删除 `mock-api.js`、`node-state.js`、`playback-tasks.js` 的脚本引用与全局使用。
- 不引入新模块文件；修改原有文件。

## 1. 阶段划分

### P1 去双源，打通真实读
- 页面启动异步拉 `/api/v1/node/info`，`nodeId = info.data.id`，`nodeSnapshot = info.data`。
- 能力：`DvsHardwareCapabilities.resolve(nodeSnapshot)`（capabilities 来自 info）。
- 配置状态：从 `nodeSnapshot.configStates` 初始化 6 个 scope 的 UI 状态；角色/待切换从 `info.data`。
- 删除 `DvsPlaybackTasks.restorePrototypeTasksForNode`；当前播放改为 `nodeSnapshot.currentPlayback`。
- 删除脚本标签：`node-state.js`、`playback-tasks.js`、`mock-api.js`（保留 real-api.js）。
- 移除 `now()` 假时间戳，使用 `new Date()` 或后端 `lastHeartbeat`/`updatedAt`。

### P2 真实写 + 状态机
- 配置保存：POST `/api/v1/node/config/:scope` `{desiredValue, operationId}`，用返回 cfg 更新 UI；删除 `applyConfig` 的 setTimeout 自演，改为轮询 GET 反射后端 state。
- 失败/重试：POST `/retry`，重渲染。
- 角色切换：POST config/role `{desiredValue:{role}}`，后端置 `pendingRole`；重启调 `/api/v1/node/reboot`（真实），重启后重拉 info。
- 维护：重启/出厂/升级调真实接口；升级需要文件选择 + SHA256 展示（文件读取计算 SHA256 于前端或后端；当前后端 upgrade 只接收 filename+sha256，前端提供文件选择并计算哈希）。
- 密码：表单接 POST `/api/v1/node/password`；登录后若 `requirement.passwordChangeRequired` 强制改密。

### P3 播放/媒体/审计
- 媒体状态：拉 `/api/v1/node/media/status` 填充会话/解码/图层/PT。
- 播放：节点页只投影 `currentPlayback`；移除任务列表操作（或保留只读展示，调用后端无接口则禁用）。
- 审计：后端 `/api/v1/audit-events` 为空桩；前端仅展示空列表，待后端补齐。

### P4 品牌/文案/文档
- 替换 ITC 品牌为 DVS：`itc-adapter.js`、`assets/brand`。
- 清除"原型/演示"字样；重写 `开发交付说明.md`。

## 2. 文件级改动清单

- `web/src/packs/node-admin/node-admin.html`：主改动。
- `web/src/_shared/real-api.js`：补 `requestAsync` 已有；无需大改，但 ensure cookie credentials（已有）。
- 删除引用：`mock-api.js`、`node-state.js`、`playback-tasks.js`（文件可保留但不加载；为避免 localStorage 真源泄漏，推荐删除文件）。
- `web/src/_shared/itc-adapter.js` / `itc-adapter.css`：品牌替换。
- `web/src/开发交付说明.md`：重写。

## 3. 风险

- 双源冲突：改造过程中 grep `DvsNodeState`/`DvsPlaybackTasks`/`localStorage` 清零。
- 状态映射：后端 `pending_apply` → UI "待生效/应用中"；`applied` → "已生效"；`failed` → "失败"；`idle` → "未保存"。
- 重启 UX：真实 reboot 后页面短暂断连，需轮询重连。
- 升级哈希：前端需计算 SHA256（Web Crypto）或后端接收文件；当前后端仅接收 filename+sha256，故前端计算。

## 4. 验收

- `DVS_REQUIRE_AUTH=1` 下登录/改密/配置/维护全链路可用。
- 无 localStorage 真源：grep 清零。
- 无"原型/演示"字样（除历史文档）。
- 品牌 DVS。
