# dvs URL 设置一览

> 对象：`app/dvs_agent/`、`app/dvs_platform/`、`app/dvs_common/` 的 HTTP 路由、静态根、端口、环境变量与端间调用。设计依据见 `docs/plan/dvs/DVS_URL_ROUTING_DESIGN.md`。本文只记录现状，不做需求论证。

## 1. 源站与静态根

| 端 | 默认端口 | 根 `/` | 静态根 | 接口前缀 |
|---|---|---|---|---|
| `dvs_agent` | `8080`（`--port` 可改） | `web/src/index.html` 跳转 `pages/node-admin/node-admin.html` | `DVS_WEB_ROOT` / `--web-root`，缺省 `./web/src` | `/api/` |
| `dvs_platform` | `8081`（`--port` 可改） | `web/src/index.html` 跳转 `pages/platform/group-list.html` | 同上，缺省 `./web/src` | `/api/` |

分发顺序两端一致：API 路由先行，落空才回退静态文件（`serve_static_file`）。`/` 映射 `index.html`，静态映射禁止 `..` 与反斜杠，无跨源头与预检处理。

前端请求封装两端同款（`web/src/_shared/api.js`）：基准地址为空、同源 `fetch`、凭据 `same-origin`。浏览器不跨源调对方接口；平台设备列表经 `agentBase` 超链新窗口打开板端根（`target=_blank rel=noopener`），见 `device-list.html` 的 `agentRoot`。

## 2. agent 接口

### 2.1 鉴权（`agent_auth.cpp`）

| 方法 | 路径 |
|---|---|
| POST | `/api/v1/node/login` |
| POST | `/api/v1/node/password` |
| POST | `/api/v1/node/logout` |

`DVS_REQUIRE_AUTH=1` 才开启 cookie 墙，默认放行；平台类调用走 `Authorization: Bearer`。

### 2.2 配置（`agent_config_routes.cpp`，scope 共 6 个：`role/input/output/network/registration/sip`）

| 方法 | 路径 |
|---|---|
| POST | `/api/v1/node/config/sync`（平台下发，仅收 `geometry/role`） |
| GET/POST | `/api/v1/node/config/<scope>` |
| POST | `/api/v1/node/config/<scope>/retry` |

### 2.3 媒体（`agent_media_routes.cpp`，代理转 IPC）

| 方法 | 路径 |
|---|---|
| GET | `/api/v1/node/media/status`、`events`、`lanes`、`tx`、`sources`、`logs` |
| POST | `/api/v1/node/media/prepare`、`commit`、`abort`、`layout` |

### 2.4 系统（`agent_sys_routes.cpp`）

| 方法 | 路径 |
|---|---|
| GET | `/api/v1/node/info`、`role`、`capability`、`capacity`、`version` |
| POST | `/api/v1/node/register`、`reboot`、`factory-reset`、`upgrade`、`upgrade/upload` |
| GET | `/api/v1/audit-events` |

### 2.5 单机垫片（`local_shim.cpp`，无平台时自洽）

| 方法 | 路径 |
|---|---|
| GET | `/api/v1/nodes` |
| POST | `/api/v1/nodes/heartbeat`、`/api/v1/nodes/upgrade` |
| GET/PUT | `/api/v1/nodes/<id>` |
| POST | `/api/v1/nodes/<id>/heartbeat`、`/api/v1/nodes/<id>/retire` |

## 3. platform 接口

### 3.1 节点网关（`node_routes.cpp`）

| 方法 | 路径 |
|---|---|
| POST | `/api/v1/nodes/register`、`/api/v1/nodes/heartbeat` |
| GET | `/api/v1/nodes` |
| GET/PUT | `/api/v1/nodes/<id>` |
| GET | `/api/v1/nodes/<id>/current-task` |
| POST | `/api/v1/nodes/<id>/heartbeat`、`/retire`、`/config-push` |

### 3.2 操作屏障与升级（`op_routes.cpp`、`upgrade_jobs.cpp`）

| 方法 | 路径 |
|---|---|
| POST | `/api/v1/ops/prepare`、`commit`、`config-ack`、`upgrade-progress` |
| GET | `/api/v1/ops/<id>`（支持 `?wait=1&timeoutMs=` 长轮询） |
| POST | `/api/v1/nodes/upgrade` |
| GET | `/api/v1/nodes/upgrade/<taskId>`（进度查询） |

### 3.3 文档库（`docs_routes.cpp`，`GET /api/v1/<kind>` 列表，`GET/POST/PUT /api/v1/<kind>/<id>` 单件）

kind 共 6 个：`groups`、`walls`、`scenes`、`bindings`、`whitelist`、`playback-tasks`。删除校验、软删除、幂等键行为见各校验函数。

### 3.4 媒体辅助（`media_routes.cpp`）

| 方法 | 路径 |
|---|---|
| POST | `/api/v1/scenes/compare` |
| POST | `/api/v1/walls/<id>/restore` |
| GET/POST | `/api/v1/walls/<id>/base-image` |
| GET | `/api/v1/walls/<id>/base-image-version` |
| POST | `/api/v1/playback-tasks/<id>/start\|cancel\|finish` |
| POST | `/api/v1/whitelist/allocate` |

### 3.5 其他

| 方法 | 路径 | 位置 |
|---|---|---|
| GET | `/api/v1/audit-events` | `platform_server.cpp` |
| WS | `/api/v1/channel/ws`（与 HTTP 同端口，握手后接管 fd） | `main.cpp` 登记，`ws_channel.cpp` 接管 |

非 GET 接口在设置令牌时要求 Bearer（`platform_server.cpp` 统一门禁）。

## 4. 端间调用

基地址与路径拼接统一用 `dvs::url_join`（`dvs_common/dvs_http_json.h`），吸收末尾斜杠。`agentBase` 由注册上报、平台原样存储，是下发与跳转的唯一真源。

| 方向 | 目标 | 传输 |
|---|---|---|
| agent → platform | `/api/v1/nodes/register`、`/heartbeat` | 短连接（`registration.cpp`），WS 可用时走通道 |
| agent → platform | `/api/v1/ops/commit`、`config-ack`、`upgrade-progress` | WS 优先、短连接回退 |
| agent → platform | `/api/v1/channel/ws`（首包 hello 携带注册体） | WS 长连接 |
| platform → agent | `/api/v1/node/media/layout` | WS 优先、短连接回退（`layout_fanout.cpp`） |
| platform → agent | `/api/v1/node/config/sync` | 同上（`node_routes.cpp` 的 `config-push`） |
| platform → agent | `/api/v1/node/upgrade` | 同上（`upgrade_jobs.cpp`） |

## 5. 环境变量与启动参数

| 项 | agent | platform |
|---|---|---|
| 端口 | `--port`，缺省 `8080` | `--port`，缺省 `8081` |
| 静态根 | `--web-root` / `DVS_WEB_ROOT`，缺省 `./web/src` | 同左 |
| 对端地址 | `--platform-url` / `DVS_PLATFORM_URL`（空 = 本地自治） | 无（被动接收注册） |
| 自报基地址 | `DVS_AGENT_BASE`，缺省按本机网络配置拼 `http://<ip>:<port>` | 存储于各 node 文档的 `agentBase` |
| 媒体通道 | `--media-sock` / `DVS_MEDIA_SOCK` | 无 |
| 令牌 | `DVS_API_TOKEN`（空 = 平台类调用不校验） | 同左（空则告警并放行变更接口） |
| 本地库 | `--db`，缺省 `/var/lib/dvs_agent/dvs_agent.db` | `--db`，缺省 `/var/lib/dvs_platform/dvs_platform.db` |

## 6. 反向代理规则

按主机名或端口分流、分别透传到两个源站的根，不做路径改写与子路径挂载。前端使用绝对路径，后端拼接假设基地址即源站根；任何前缀挂载都会同时破坏两者。平台流式路径与普通接口同端口，代理层不可截断升级握手。
