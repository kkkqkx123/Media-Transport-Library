# DVS 前端:原型成分分析 vs 正式实现改造方案

> 分析对象:`app/dvs_agent/web/src/`(`node-admin` 节点设备后台及其 `_shared` 适配器)。
> 目的:厘清哪些是**原型/演示专用**成分,以及**正式上线前需要如何改造**。
> 基线:`feat/rk3588-apps` 分支。后端(dvs-agent C++)已实现的接口见 §2,前端尚未完整对接。

---

## 0. 结论速览

| 模块 | 当前原型实现 | 能否直接用于正式 | 正式化改造要点 |
|---|---|---|---|
| 认证 | `mock-api.js` 硬编码 `admin/admin123` | ❌ | 已由 `real-api.js` 对接 `/api/v1/node/login`(后端 PBKDF2 校验 + 限速),需补会话/401/首次改密流程 |
| 节点数据(信息/状态) | `node-state.js`(DvsNodeState)本地 localStorage 7 节点种子 | ❌ | 删除本地数据源,改从 `/api/v1/node/info` 取数 |
| 六 scope 配置状态机 | 页面 `applyConfig()` 用 `setTimeout` 模拟 `pending→applying→applied` | ❌ | 改 POST `/api/v1/node/config/:scope` + 轮询 GET 反射后端真实状态 |
| 角色切换 / 输入口 / 输出 / 网络 / SIP | 页面 `patchNode()` 直接写 localStorage,自管 `configStates` | ❌ | 走真实配置接口,状态以后端 `configRevision` 为准 |
| 维护(重启/出厂/升级) | 仅 `patchNode()` 改本地运行态,**不调后端** | ❌ | 调 `/api/v1/node/maintenance/{reboot,factory_reset,upgrade}`;升级需真文件 + SHA256 |
| 媒体状态(会话/解码/图层) | 页面未取,无展示 | ⚠️ 部分 | 接 `/api/v1/node/media/{state,stats}` |
| 播放任务 | `playback-tasks.js` 本地 7 任务种子 + `restorePrototypeTasksForNode` 注入 | ❌ | 节点端只展示 `node.currentPlayback`(来自 info);完整任务列表依赖**控制服务**(平台侧,节点 agent 无此接口) |
| 硬件能力 | `hardware-capabilities.js` 基线 + 以 `node.capabilities` 覆盖(基本正确) | ✅ 可保留 | 去掉对演示种子的依赖,完全用后端 `capabilities` |
| 适配层 | `mock-api.js`(localStorage) + `real-api.js`(fetch) | ⚠️ | 删除 `mock-api.js`,`real-api.js` 作为唯一适配层并补全端点 |
| 品牌/文案 | ITC 品牌 logo、演示账号、页面多处"原型"字样 | ❌ | 换 DVS 品牌,移除演示/原型标识,重写 `开发交付说明.md` |

**一句话结论**:`real-api.js` 已接通,但页面**只**把"登录"和"播放任务操作"两类请求送进了适配器;节点信息、配置状态机、维护、媒体**全部仍走本地 mock 与客户端模拟**,因此当前前端**不是**可交付的正式实现,需按 §3 系统改造。

---

## 1. 原型/演示专用成分盘点

### 1.1 适配器层(数据契约模拟)
- **`web/src/_shared/mock-api.js`**:整文件为原型。用 `localStorage` 模拟节点/任务/审计/登录(`admin/admin123` 写死),并自行拼装示例响应。生产必须删除。
- **`web/src/_shared/real-api.js`**:已实现 `fetch` + `Cookie` 转发,**方向正确**,但当前仅作为"替换层"被页面用于 `login` 与 `playback` 两类请求;其余端点未覆盖(页面根本没调用)。

### 1.2 本地数据源(真源是浏览器 localStorage)
- **`web/src/_shared/node-state.js`(`DvsNodeState`)**:含 7 个节点的**写死种子**(SN `RK3588-2210-00xx`、`fw: V1.0.0-20261019`、`192.168.10.x` 等),并提供 `get/list/update/heartbeat` 读写 `localStorage`。这是页面节点数据的"假真源"。
- **`web/src/_shared/playback-tasks.js`(`DvsPlaybackTasks`)**:含 7 条写死播放任务种子,`restorePrototypeTasksForNode()` 在页面加载时**主动把演示数据注入**本地存储。

### 1.3 页面内模拟逻辑(`packs/node-admin/node-admin.html`)
- **`nodeId = "d1"` 硬编码**(line 520):本地节点 id 写死为演示值,而非从后端取。
- **`DvsNodeState.get(nodeId)` 作为渲染数据源**(line 522):`nodeSnapshot` 来自本地存储,非后端。
- **`patchNode()` / `patchNodeConfig()`**(line 622–631):所有保存动作只写 localStorage,**不触达后端**。
- **`applyConfig()` 用 `window.setTimeout` 模拟状态迁移**(line 702–725):客户端自演 `pending→applying→applied/failed`,**与后端状态机完全脱钩**。
- **`now()` 返回假时间戳 `"2026-10-19 16:5" + random`**(line 681):"最近动作"时间全是伪造演示时间。
- **维护按钮只 `patchNode()`**(line 1196/1238/1307):点重启/出厂/升级仅改本地运行态并弹 toast,**不发任何后端请求**。
- **`protoConfirm`** 多处用于二次确认(UX 组件,可保留,但文案需正式化)。

### 1.4 演示文案 / 品牌 / 文档
- `itc-adapter.js` 注入 **ITC** logo、页脚版权、平台菜单(对 node-admin 页是 no-op,但对平台页生效)。
- `assets/brand/ITC Design-logo-write.svg` 为 ITC 品牌资源。
- `开发交付说明.md` 明确写"本地模拟数据""不具备生产安全能力"。
- 页面/JS 注释多处"原型用""演示"。

### 1.5 播放任务(无后端)
- 节点 agent **没有** `/api/v1/playback-tasks` 接口;播放任务属**控制服务**(平台侧)。原型用本地种子模拟。

---

## 2. 后端已具备的真实能力(前端可对接,见 `app/dvs_agent/main.cpp` 路由)

| 后端接口 | 实现状态 | 前端当前是否调用 |
|---|---|---|
| `POST /api/v1/node/login` | ✅ PBKDF2 校验 + 5次/60s 限速 | ✅ 通过 `DvsMockApi`(real 模式生效) |
| `POST /api/v1/node/password` | ✅ 旧密码校验 + 改密 + 清 `password_change_required` | ❌ 页面有表单但未接此接口 |
| `GET /api/v1/node/info` | ✅ 返回节点全量(含 `online`/`currentPlayback`/`metrics`) | ❌ 页面用 `DvsNodeState` 而非它 |
| `GET /api/v1/node/capability` | ✅ 返回 `capabilities` | ❌ 页面用本地基线 |
| `GET/POST /api/v1/node/config/:scope`(+`/retry`) | ✅ 六 scope 状态机(desired/applied/pending/failed),落 SQLite | ❌ 页面用本地 `applyConfig` 模拟 |
| `GET /api/v1/node/media/{state,stats}` | ✅ 会话/解码/图层/PT 状态 | ❌ 未取 |
| `POST /api/v1/node/maintenance/{reboot,factory_reset,upgrade}` | ✅ reboot 落 pendingRole、factory 保网络/注册、upgrade 做 SHA256+A/B | ❌ 页面只改本地态 |
| `GET /api/v1/audit-events` | ⚠️ 桩(返回空) | ❌ |
| `POST /api/v1/playback-tasks/...` | ❌ 节点 agent 无此接口 | ✅ 走 `DvsMockApi`,但真实模式会 404 |

> 说明:节点 agent 的 `Registration` 负责向**平台**做 register/heartbeat(HTTP 轮询,平台为 stub);节点**页面对平台多节点接口 `/api/v1/nodes/*` 不依赖**(node-admin 是单机本机页)。

---

## 3. 正式化改造方案(文件级)

### 3.1 清理适配层与本地数据源
- 删除 `mock-api.js`、`node-state.js`、`playback-tasks.js`(及其种子数据);或保留为"dev demo"模式,通过 `index.html` 是否加载它们来切换(推荐:**默认不加载**,避免任何 localStorage 真源泄漏)。
- `real-api.js` 作为唯一适配层:补全缺失端点(`password`、`config` GET/POST、`media`、`maintenance`、`capability`、`info`、审计),并统一 `request`/`requestAsync` 契约。
- 风险:若保留 demo 模式,务必确保页面在真实模式下**不引用** `DvsNodeState`/`DvsPlaybackTasks`,否则会与后端数据双源冲突。

### 3.2 节点数据改从后端取(去本地真源)
- 页面启动:`GET /api/v1/node/info` → 渲染 `syncNodeView()` 的数据源由 `nodeSnapshot = DvsNodeState.get()` 改为 `info.data`。
- `nodeId` 不再硬编码 `"d1"`,改为从 `info.data.id` 取(本机节点)。
- 删除 `DvsNodeState.subscribe` 本地事件驱动;改为轮询或 S3 的 WS 推送。

### 3.3 配置状态机改走真实接口 + 轮询
- 保存:`POST /api/v1/node/config/:scope` 带 `{desiredValue/value, operationId}`;用返回里的 `desiredRevision` 展示"待生效"。
- 状态反射:**轮询** `GET /api/v1/node/config/:scope`,按后端 `state`(`pending_apply`/`applied`/`failed`)渲染,**删除** `applyConfig()` 的 `setTimeout` 自演。
- 失败/重试:调 `/retry` 并重渲染;失败原因用后端 `errorCode/errorReason`。
- 角色切换:提交 `role` 后,后端置 `pendingRole`;**重启由维护接口真实执行**,页面在 reboot 后重新拉取 `info` 反映新 `role`。

### 3.4 维护操作调真实接口
- 重启:`POST /api/v1/node/maintenance/reboot`(后端先落实 `pendingRole`);页面预期节点短暂不可达,**刷新或等待后重新拉 `info`**。
- 出厂:`POST /api/v1/node/maintenance/factory_reset`(后端保留网络/注册、重置角色/输入/输出/SIP/密码);前端清本地缓存并回登录。
- 升级:`POST /api/v1/node/maintenance/upgrade` 需**真实固件文件 + 期望 SHA256**(页面要提供文件选择与哈希展示,不能再用 localStorage 假 `fw`);A/B 与失败回滚由后端保证。

### 3.5 媒体 / 能力 / 注册状态来自后端
- `/api/v1/node/media/{state,stats}` 填充"会话数/解码中/图层/PT 状态"。
- `/api/v1/node/capability` 与 `node.capabilities` 驱动 `hardware-capabilities.js` 的 `resolve()`(去除对演示种子的任何 fallback 依赖)。
- 注册状态(online/registerState/registerMessage)全部取自 `info`。

### 3.6 播放任务:节点端只投影,列表交控制服务
- 节点页"当前播放"=`info.data.currentPlayback`(后端从任务在节点上的运行态投影),无需本地任务源。
- 完整"播放任务"列表/创建/检查/执行是**控制服务(平台侧)**职责,节点 agent 暂无接口。正式方案二选一:
  1. 节点页仅展示当前播放 + 历史(来自 info / 控制服务提供的节点视角接口);或
  2. 后端补 `/api/v1/node/playback/*` 代理到控制服务(超出 S2 范围,建议 S3/S4)。
- 删除 `restorePrototypeTasksForNode` 与本地任务种子。

### 3.7 去除伪造时间
- 删除 `now()` 假时间戳;用 `new Date()` 或后端返回的 `lastHeartbeat`/`updatedAt`(ISO8601)格式化展示。

### 3.8 登录态 / 会话 / 首次改密
- `real-api.js` 已带 `Cookie` 与 `credentials`;页面需:
  - 401 / 未登录 → 跳转登录;登录成功缓存 `passwordChangeRequired`,若 `true` 强制弹改密框;
  - 退出登录:调用后端登出(或清本地会话态)并重定向登录页;
  - 改密表单接 `POST /api/v1/node/password`(§3.1 已列)。

### 3.9 品牌 / 文案 / 文档
- 替换 `itc-adapter.js` 的 ITC logo/版权/菜单为 **DVS** 品牌;替换 `assets/brand` 资源。
- 清除页面与 JS 中所有"原型/演示"字样;`开发交付说明.md` 改写为正式交付说明(真实登录账号策略、真实接口、生产安全提示)。

### 3.10 审计(后置)
- 后端 `/api/v1/audit-events` 当前为桩;正式需后端补真实审计落库,前端再接列表展示(S3 或 S5)。

---

## 4. 改造阶段建议

1. **P1(去双源,打通真实读)**:删本地数据源,`info`/`capability`/`config` GET 接后端,渲染正确;保留 `real-api.js` 唯一适配层。此时只读视图已为真。
2. **P2(真实写 + 状态机)**:配置提交走 POST + 轮询;维护三项走真实接口;去 `applyConfig` 模拟。
3. **P3(播放/媒体/审计)**:媒体状态接入;播放改投影;审计后端补齐后再接。
4. **P4(品牌/文案/文档)**:统一 DVS 品牌,清"原型"标识,重写交付说明。

---

## 5. 风险与回归

- **双源冲突**:若未彻底移除 `DvsNodeState`,页面可能显示 localStorage 旧值覆盖后端真值。改造时全局 grep `DvsNodeState`/`DvsPlaybackTasks`/`localStorage` 清零。
- **状态机语义**:后端 `state` 取值为 `idle/pending_apply/applied/failed`,与页面旧标签 `pending/applying` 不同,渲染映射需对齐(`setConfigState` 标签表 line 697)。
- **重启 UX**:真实 reboot 会导致页面与节点短暂断连,需有"等待重启→自动重连重拉"的体验,而非本地假 `rebooting` 态。
- **播放缺后端**:S2 节点 agent 无播放接口,正式节点页若强依赖任务列表会 404;按 §3.6 收敛为"当前播放投影"。
- **ITC 品牌残留**:`itc-adapter.js` 与 `assets/brand` 多处引用,正式发布前需全量替换,避免误带第三方品牌。
