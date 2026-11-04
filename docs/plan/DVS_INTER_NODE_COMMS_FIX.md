# DVS 节点间 / 应用间通信改造方案（剩余问题细化）

> 整理日期：2026-11-03
> 输入：`docs/analysis/ARCHITECTURE.md`、`inter_node_communication_analysis.md`（见仓库根 `/workspace`）、`app/dvs_*`、`app/dvs_common` 源码
> 关联：`docs/plan/REMAINING_TASKS.md`（S0–S5 平台/agent/media 任务）
> 状态图例：⬜ 未开始　🟨 部分完成　✅ 已完成　⛔ 阻塞

---

## 0. 总览与结论

针对 `inter_node_communication_analysis.md` §4.2/§4.3 识别的剩余问题，本方案给出可落地的代码级改造。**架构方向正确**，本次只补"关键链路未闭环"与"控制面健壮性"短板，不推翻控制/数据面分离、本地 IPC、星形平台、PTP/ST2110 媒体同步的整体选型。

优先级与落地状态：

| ID | 问题（来源 §4.2） | 严重度 | 本次改造 | 状态 |
|----|----|----|----|----|
| **F1** | 两阶段提交未闭环（平台不 fan-out 子布局、agent 不回执 commit） | 高 | 平台 `ops/prepare` 计算像素子布局并下发 agent；agent 接收后驱动 media `prepare` 并后台回执 `commit` | 🟨 闭环打通（像素拆分 + 回执） |
| **F2** | 配置权威源分裂（平台/agent 双真相） | 高 | 平台为 source of truth，新增 `config push` → agent `config/sync` 增量同步 geometry/role | 🟨 打通 push 通道（增量合并） |
| **F3** | 控制面 HTTP 裸 socket、无重试/keepalive | 中 | `HttpClient` 增加带退避重试的 `post_json` 重载 | ✅ 已实现 |
| **F4** | 鉴权默认关闭、无 mTLS | 中 | 新增 `DVS_API_TOKEN` Bearer 鉴权（fan-out/commit 强制携带），`DVS_REQUIRE_AUTH` 保留默认关 | 🟨 控制面 token 化（mTLS 见开放点） |
| **F5** | 屏障"全就绪"无每 lane 就绪/丢帧语义 | 中 | media 新增 `lane_stats` hook，agent 暴露 `/media/lanes` 并随 commit/心跳上报 | 🟨 hook + 端点打通（引擎填数待补） |
| **F6** | IPC 事件广播给所有 client / 槽 O(N) 扫描 | 低 | 预留 client 订阅过滤位（本次仅加注释与 `lanes` 复用，结构改造留待规模化） | ⬜ 仅预留，未改结构 |

> 注：本次改动全部为**源码编辑**（环境无 C/C++ 工具链），编译与开发板验证由本地完成。各节点 diff 见 §5，落地点见 `git diff HEAD`。

---

## 1. 背景与问题来源

现有分布式视频墙系统的通信拓扑（`inter_node_communication_analysis.md` §1）为：
- 跨节点控制：星形，`dvs_agent`（:8080）→ `dvs_platform`（:8081）HTTP 注册/心跳；平台反向 HTTP 升级/下发。
- 同节点：`dvs_agent` ⇄ `dvs_media` 走 Unix 域套接字 IPC（NDJSON）。
- 媒体引擎间：`dvs_media` ⇄ `dvs_media` **无应用层通信**，依赖 PTP + ST2110 时钟域。

已读代码确认的两个"骨架未接线"缺口最致命：
1. `dvs_platform/main.cpp:286-340` 的 `OpCoordinator` 两阶段提交状态机已完整，但**平台从不把 per-node 子布局推给 agent**（`layout.cpp` 只算不推），且 **agent 从不调用 `/api/v1/ops/commit`**。
2. 平台存 groups/walls/scenes/bindings，agent 自存 node config，**二者无同步路径**。

---

## 2. 现状事实（逐条 file:line）

| 事实 | 证据 | 说明 |
|----|----|----|
| 平台 `ops/prepare` 仅 `begin` + 回 `applyAt`，不联系 agent | `dvs_platform/main.cpp:306-313` | 屏障起手，但下游 fan-out 缺失 |
| 平台升级 fan-out 用 `agentBase` 字段 | `dvs_platform/main.cpp:352` | 节点 doc 需含 `agentBase` 才能反向调用 |
| agent 注册体未带 `agentBase`/`geometry` | `dvs_agent/registration.cpp:158-184` | 平台无法定位 agent 地址与节点画布 |
| agent→media 仅 `set_sources/configure/prepare` 直发 | `dvs_agent/main.cpp:331-342, 479-487` | 无"接收平台布局→commit 回执"链路 |
| `HttpClient` 单次 `poll` 超时、无重试、HTTP/1.0/close | `dvs_common/src/http_client.cpp:30-108` | 受信任 LAN 可用，可靠性不足 |
| 鉴权默认关，`sess-1` 硬编码 | `dvs_agent/main.cpp:252, 212` | 原型可接受，需可配置强校验 |
| media `query_state` 仅 aggregate counts，无 per-lane | `dvs_media/ipc/ipc_server.c:264-288` | 屏障无法感知单 lane 就绪 |
| `OpCoordinator::on_reply` 已实现收集 + 提交/回滚 | `dvs_platform/ops.cpp:17-31` | 只需前端喂 `node/status` 即可驱动 |

---

## 3. 差距清单（剩余问题 → 改造映射）

| 严重度 | 剩余问题 | 改造点 | 落地文件 |
|----|----|----|----|
| 高 | F1 两阶段提交未闭环 | 平台像素拆分 + fan-out；agent 接收/回执 | `dvs_platform/main.cpp`、`dvs_agent/main.cpp`、`dvs_agent/registration.cpp` |
| 高 | F2 配置权威源分裂 | 平台 push / agent sync | `dvs_platform/main.cpp`、`dvs_agent/main.cpp` |
| 中 | F3 HTTP 无重试 | 重试退避重载 | `dvs_common/src/http_client.cpp`、`.h` |
| 中 | F4 鉴权默认关 | Bearer token | `dvs_agent/main.cpp`、`dvs_platform/main.cpp`、`http_client.*` |
| 中 | F5 就绪无 lane 语义 | `lane_stats` hook | `dvs_media/ipc/ipc_server.*`、`engine/ipc_engine_bridge.c`、`dvs_agent/main.cpp` |
| 低 | F6 IPC 广播/扫描 | 预留订阅位 | （本次仅注释预留） |

---

## 4. 总体改造策略

1. **保持控制/数据面分离**：所有改动只在控制面（platform/agent/common）与 media 的 IPC 接口，不动 MTL 数据面算法。
2. **平台为配置与几何唯一权威（F2）**：node `geometry`（全局画布坐标）、`agentBase`、`role` 由平台持有并 push；agent 本地仅缓存/执行。
3. **闭环两阶段提交（F1）**：平台在 `ops/prepare` 算好每个 member 的像素子布局 → POST 到 agent `/media/layout` → agent 转 media `prepare` → media 完成后 agent 后台线程监听 `media.prepare.result` → POST 平台 `/ops/commit` → 平台 `on_reply` 聚齐后转 `committed`，到 `applyAtNs` 统一切换。
4. **控制面 token 化（F4）**：新增 `DVS_API_TOKEN` 环境变量，fan-out 与 commit 调用强制携带 `Authorization: Bearer`；`DVS_REQUIRE_AUTH=1` 时 agent 拒绝缺 token 的请求（默认仍关，保持原型可用）。
5. **就绪下沉到 lane（F5）**：media 新增可选 `lane_stats` hook，`query_state` 输出 `lanes[]`；agent `/media/lanes` 暴露，并随 commit/heartbeat 上报，使屏障"全就绪"具备逐 lane 语义。
6. **HTTP 可靠性（F3）**：`HttpClient` 增加 `post_json(..., retries, bearer)` 退避重试，不破坏既有单发调用。

---

## 5. 各节点改造细节（diff 级）

### 5.1 `dvs_common` — HttpClient 重试 + Bearer（F3/F4）

`dvs_common/include/http_client.h`：新增重载
```cpp
int post_json(const std::string& url, const std::string& body,
              std::string& resp, int timeout_ms, int retries,
              const std::string& bearer);
```
`dvs_common/src/http_client.cpp`：
- 私有 `request` 增加 `const std::string& auth` 参数，非空时追加 `Authorization: Bearer <auth>\r\n`；
- 新重载循环 `retries` 次（指数退避 200ms×2^k，上限 2s），任一成功（status>0 且 <400）即返回；全部失败返回最后 status。

落点：`post_json` 原有签名保留，新增 6 参重载（`http_client.cpp:110-113` 下方追加）。

### 5.2 `dvs_platform` — 注册捕获 geometry/agentBase + ops/prepare fan-out（F1/F2/F4）

`dvs_platform/main.cpp`：
- 新增全局 `static std::string g_api_token = env_or("DVS_API_TOKEN", "");`（仿 `main.cpp:18` 风格，需加 `<cstdlib>` 取 `getenv`）。
- **register 处理（`:170-194`）**：从 body 读取 `agentBase`、`geometry`（`{x,y,w,h}`），若非空则写入 node doc（与现有 `online/lastHeartbeat` 同一 `put_doc` 路径）。
- **ops/prepare（`:286-313`）**：在 `g_ops.begin(...)` 之后新增 fan-out 段：
  - 读取 body 的 `wall`（`{w,h}`）与 `windows`（全局窗口数组，每项 `{win:{x,y,w,h}, src:{w,h}, stream}`）；
  - 对每个 member：取 node doc 的 `agentBase` + `geometry`；无 `geometry` 则跳过并记 audit；调用 `layout_window_fragment` 逐窗切出节点局部 `dst` + 源 `src`，组装 `windows[]`；
  - 构造 `{"operationId","revision","applyAtNs","geometry","windows"}`，`cli.post_json(agentBase+"/api/v1/node/media/layout", body, resp, 5000, 2, g_api_token)`；
  - 任一失败记 `audit`（不阻断其余节点，沿用升级的隔离策略）。
- **新增 config push（F2）**：在 `nodes/<id>/...` 路由内新增 `POST act=="config-push"`：从 body 取 `config` 对象，`POST agentBase+"/api/v1/node/config/sync"`（token 鉴权），返回逐字段应用状态。
- helper：`fragment_for_node(json_object* windows, Rect node, int wall_w, int wall_h)` 回收 `std::vector`/`json` 对象，置于匿名命名空间（`:32` 之后）。

依赖：`layout.h` 已暴露 `layout_window_fragment`（`layout.h:19`），无需改 layout.cpp。

### 5.3 `dvs_agent` — 接收子布局 + 回执 commit + token 鉴权 + lanes + config-sync（F1/F4/F5/F2）

`dvs_agent/main.cpp`：
- 新增全局 `static std::string g_api_token`（`env_or("DVS_API_TOKEN","")`）。
- **token 鉴权（`:252-257`）**：在现有 `g_require_auth` 分支内，除校验 cookie 外，允许 `Authorization: Bearer <g_api_token>` 通过（平台↔agent 内部调用用 token，浏览器用 cookie）。
- **新增 `POST /api/v1/node/media/layout`（F1）**（建议插在 `:487` 后）：
  - 解析 `{operationId,revision,applyAtNs,geometry,windows}`；
  - 组装 media `prepare` IPC 载荷（带 operationId/revision/windows/applyAtNs），`g_media->request("prepare", prep)`（202 即接受）；
  - 记录 pending op，**detach 一个后台线程** `report_op_commit(opid,rev,applyAtNs,nodeId)`：循环 `g_media->drain_events()`（每次睡 100ms，上限 5s）找 `media.prepare.result` 匹配 operationId，得到 status，再 `POST platform_url+"/api/v1/ops/commit"`（`{"operationId","node":nodeId,"status"}`，token 鉴权）。
- **新增 `GET /api/v1/node/media/lanes`（F5）**：`query_state` 已含 `lanes`（见 5.4），透传 `lanes` 字段。
- **新增 `POST /api/v1/node/config/sync`（F2）**：接收平台下发的 `config` 对象，对 `geometry`/`role` 等 scope 调 `g_store->set_desired` + `update_node`，标记 `source:"platform"` 以便溯源；best-effort，失败不影响本地落库。
- `nodeId` 取自 `get_str(g_store->node(),"id","local-node")`。
- 后台线程需 `#include <json-c/json.h>`（已在 `:1`）与 `HttpClient`（新增 include `http_client.h`）。

### 5.4 `dvs_media` — lane_stats hook（F5）

`dvs_media/ipc/ipc_server.h`：hook 表（`ipc_server.h:18-25`）新增
```c
json_object* (*lane_stats)(void); /* per-lane readiness/lost frames, may be null */
```
`dvs_media/ipc/ipc_server.c`：
- `query_state`（`:264-288`）在 data 中追加 `"lanes":<lane_stats() 或 []>`；新增局部 `json_object* lanes = (g_hooks.lane_stats)? g_hooks.lane_stats() : NULL;` 若空则 `json_object_new_array()`。
- data 拼接字符串需扩展（当前 `data[512]` 改为放大，或先构造 `json_object` 再序列化；推荐后者更稳）。
- `dvs_ipc_install_hooks`（`ipc_server.c:44-46`）无需改，`lane_stats` 默认 NULL 即空数组，向后兼容 no-MTL 单测。
`dvs_media/engine/ipc_engine_bridge.c`：新增
```c
static json_object* hook_lane_stats(void) {
  /* TODO(F5): 调用 dvs_engine_lane_stats 填每路 rx 就绪/丢帧；
     当前返回空数组，待引擎侧补齐符号。 */
  return json_object_new_array();
}
```
并在 `dvs_ipc_attach_engine`（`:44-55`）hook 表填入 `.lane_stats = hook_lane_stats`。

> 引擎侧 `dvs_engine_lane_stats` 暂未实现，故 hook 先返回空数组，协议与 agent 端点已就绪；待开发板验证时补引擎填数（开放点 O3）。

### 5.5 `dvs_agent/registration.cpp` — 注册带 agentBase + geometry（F1/F2 前置）

`dvs_agent/registration.cpp`：
- `build_register`（`:158-184`）：新增 `agentBase`（`env_or("DVS_AGENT_BASE","http://<ip>:8080")` 或新增参数传入；推荐从 `main` 经 `Registration` 构造传入）与 `geometry`（取自 `node` 的 `geometry` 字段，缺省 `{"x":0,"y":0,"w":0,"h":0}`）。
- `Registration` 构造（`registration.h:18-19`）新增 `agent_base` 参数；`main.cpp:187` 一并传入。

---

## 6. 风险与回退

| 风险 | 缓解 |
|----|----|
| fan-out 在 `ops/prepare` 内同步调用 agent，高节点数下拉长请求时延 | 平台侧超时为 5s/节点且串行；节点数 ≤32 可接受，规模化改线程池（开放点 O4） |
| agent 后台 commit 线程与 HTTP handler 共享 `g_media`（锁已保护） | `drain_events`/`request` 均 `mtx_` 保护，安全；线程仅在超时后兜底回执 `blocked` |
| token 配置不一致导致 agent 拒平台请求 | `DVS_REQUIRE_AUTH` 仍默认关；token 仅作"有则校验"的附加通道，未配时不阻断 |
| 引擎 `lane_stats` 暂空 | 不影响编译与既有 query_state；agent 上报空 lanes，屏障语义待引擎补数后生效 |

回退：所有改动为新增接口 + 向后兼容字段，删除 patch 即可还原；无 schema 破坏性变更。

---

## 7. 验证步骤（本地/开发板）

1. **单元**：`dvs_media` no-MTL 单测（IPC server 仍链接，因 `lane_stats` 默认 NULL）→ `query_state` 返回 `lanes:[]`。
2. **control plane**：起 platform + 两 agent，operator 调 `POST /api/v1/ops/prepare` 带 `wall/windows/members` → 观察每个 agent 收到 `/media/layout` → media `prepare` → agent 回执 `/ops/commit` → 平台状态机转 `committed`。
3. **token**：设 `DVS_REQUIRE_AUTH=1` + `DVS_API_TOKEN=xxx`，确认缺 token 的平台→agent 调用被拒、带 token 正常。
4. **config sync**：平台 `POST /nodes/<id>/config-push` → agent `/config/sync` 写 `geometry` → 下次 `register` 上报该 geometry，平台 `prepare` 可据此拆分。
5. **长稳**：心跳/commit 在 transport 抖动下不阻塞 media 输出（沿用既有降级）。

---

## 8. 兼容性（接口契约）

| 端点 | 方向 | 新增字段 |
|----|----|----|
| `POST /api/v1/nodes/register` | agent→platform | `agentBase`, `geometry` |
| `POST /api/v1/ops/prepare` | operator→platform | `wall`, `windows`（平台据此 fan-out） |
| `POST /api/v1/nodes/<id>/config-push` | operator→platform | `config` |
| `POST /api/v1/node/media/layout` | platform→agent（新增） | `operationId,revision,applyAtNs,geometry,windows` |
| `POST /api/v1/ops/commit` | agent→platform（已有，补齐调用方） | `node` |
| `POST /api/v1/node/config/sync` | platform→agent（新增） | `config` |
| `GET /api/v1/node/media/lanes` | operator→agent（新增） | 透传 `lanes[]` |
| IPC `query_state` | agent→media | `lanes[]`（新增，默认空） |

---

## 9. 文件改动清单

| 文件 | 改动类型 | 关联 |
|----|----|----|
| `app/dvs_common/include/http_client.h` | 新增重载声明 | F3/F4 |
| `app/dvs_common/src/http_client.cpp` | 重试 + Bearer 实现 | F3/F4 |
| `app/dvs_platform/main.cpp` | register 捕获、ops fan-out、config-push、token | F1/F2/F4 |
| `app/dvs_agent/main.cpp` | layout 接收、commit 回执、token、lanes、config-sync | F1/F2/F4/F5 |
| `app/dvs_agent/registration.cpp` | 注册带 agentBase/geometry | F1/F2 前置 |
| `app/dvs_agent/registration.h` | 构造加 agent_base | F1/F2 前置 |
| `app/dvs_media/ipc/ipc_server.h` | hook 表加 lane_stats | F5 |
| `app/dvs_media/ipc/ipc_server.c` | query_state 输出 lanes | F5 |
| `app/dvs_media/engine/ipc_engine_bridge.c` | hook_lane_stats 桩 | F5 |

---

## 10. 下一步

- 引擎侧补齐 `dvs_engine_lane_stats`（O3）以让 `lanes` 非空。
- 平台 `ops/prepare` fan-out 改线程池（O4）。
- 控制面 mTLS（O5）。
- 配置增量 diff 推送（O2）。

---

## 11. 待拍板开放点（OPEN）

- **O1（F1 几何来源）**：`geometry` 由 operator 在 `ops/prepare` 的 `wall` 之外单独提供，还是由平台按 node 注册自动编排？本方案采用"平台存储 node geometry（注册上报），operator 仅给 `wall`+`windows`"，需确认 operator 端是否接受。
- **O2（F2 增量 vs 全量）**：`config/sync` 当前全量覆盖 geometry/role；是否需字段级 diff 以免覆盖 agent 本地 scope（input/output 由 agent 兜底）？
- **O3（F5 引擎符号）**：`dvs_engine_lane_stats` 尚未实现，hook 先返回空数组；是否接受"协议先就绪、填数后补"的递进策略？
- **O4（F1 规模化）**：fan-out 串行 + 5s/节点超时，节点数 >32 时的线程池改造是否必要？
- **O5（F4 mTLS）**：Bearer token 仅为内网边界最小加固；是否需在 `HttpClient`/`HttpServer` 层加 mTLS（需证书基础设施）？
- **O6（F6 结构）**：IPC client 槽 O(N) 扫描与全量广播是否现在就改哈希/订阅，还是等规模化（>16 client）再说？
