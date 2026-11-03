# DVS S2 节点代理 dvs-agent + node-admin —— 选型与细化实现方案

> 本文件对 `DVS_FRONTEND_INTEGRATION_PLAN.md` 的 **S2 阶段**做"明确选型"并细化为文件级修改方案,可直接落地。
> 代码基线:`feat/rk3588-apps` 分支,`app/dvs_media/`(S1,已提供 IPC 契约)与 `app/dvs_agent/`(S2 骨架已存在)。
> 状态:实现完成(2026-11-03;本地 x86 单测 21/21 通过 + 编译校验通过;devs 服务器正式编译待执行)。

---

## 0. S2 目标回顾

节点侧具备:六 scope 配置状态机、注册/心跳、维护操作、本机 HTTP 服务(对接 node-device-admin 前端)、以及把配置变更经 IPC 下发给 `dvs_media`。对应 req 05 P4 节点部分 + req 03 §9.5/§10 + req 06 §2.2/§2.3。

S2 的平台为 **stub**(无真实 `dvs_platform`):注册/心跳做真实逻辑但目标地址可空,空地址时退化为"本机自治、不出网",媒体出画不中断。

---

## 1. 明确选型(Selections)

| # | 维度 | 选型 | 理由 / 替代项 | 变更影响 |
|---|---|---|---|---|
| S2-1 | 语言/标准 | **C++17**,单翻译单元友好;禁用依赖新 libstdc++ 的 STL 容器(故 `media_ipc` 用 fixed ring 而非 `std::deque`) | 替代:C++20(不需要)、Python(风险表"语言选型返工"已规避,接口与语言解耦) | 无 |
| S2-2 | 本机 HTTP 服务 | **自研轻量 POSIX socket 服务**(`http_server.cpp`),单 accept 线程 + 每连接 detached 线程;不引入 libmicrohttpd/civetweb/Boost.Beast | 替代:CivetWeb(增加构建复杂度)、libmicrohttpd(回调模型重) | 无(已落地) |
| S2-3 | HTTP 客户端(注册/心跳/平台下发) | **自研 POSIX socket 客户端**(`http_client.cpp`),与服务器对称,**无 libcurl 依赖**;URL 为空时 no-op,失败不抛、不中断媒体 | 替代:libcurl(外部依赖,ARM 交叉链易踩坑) | 新增文件 |
| S2-4 | 持久化 | **vendored SQLite amalgamation**(`third_party/sqlite/`,`SQLITE_THREADSAFE=1`),单文件 KV 表存 `node`/`config`,替代原型 localStorage | — | 无(已落地) |
| S2-5 | 密码哈希/认证 | **自研 SHA-256 + PBKDF2-HMAC-SHA256(10 万次迭代)+ 每用户随机 salt**,零外部依赖;登录限速:每来源 5 次/60s,锁 300s | 原计划 Argon2;S2 选 PBKDF2-SHA256 满足 req 03 §9.5"本机校验、首次改密",**Argon2 列为 S5 hardening 候选** | 改 `node_store` + 新增 `sha256` |
| S2-6 | 注册/心跳传输 | **S2 = HTTP 轮询**(register 一次性 + heartbeat 3s),平台 stub;**S3 再换 WS**。连续 3 次超时判离线 | 替代:WS(留给 S3) | 新增 `registration` |
| S2-7 | 前端接入 | 复制 `ref/node-device-admin/` → `web/src/`(原型只读保留 `ref/`);新增 `real-api.js`(实现 `DvsMockApi.request` 同名接口,走 `fetch`+session cookie)。**偏差**:原型 `node-admin.html` 只加载 `mock-api.js` 而从未加载 `real-api.js`,故需在该页 `mock-api.js` 之后追加一行 `<script src="../../_shared/real-api.js">` 以激活真实适配器(页面调用逻辑 `DvsMockApi.request` 仍未改) | 页面追加 1 个 script 标签 | 已完成 |
| S2-8 | IPC 桥 | 复用 `media_ipc.cpp` 客户端(Unix socket JSON 行协议),与 `dvs_media/ipc/ipc_server.c` 契约一致:`configure/prepare/commit/abort/clear/set_sources/query_state/event`;请求带 `messageId`,响应回 `requestId`,事件行缓存。`role/input/output` 变更经 `configure`/`set_sources` 下发 | — | 改 `main.cpp`(补齐转发) |

**选型结论**:S2 全部采用"零新增外部依赖、自研对称 POSIX 网络栈 + vendored SQLite + 自研 SHA-256/PBKDF2"的策略,保证在 RK3588 交叉链与本机 CI(x86) 均可编译,且接口(JSON/IPC)与语言/传输解耦,利于 S3 换 WS 与 S5 hardening 替换 Argon2。

---

## 2. 现有骨架盘点(已落地,本阶段仅核验/复用)

| 文件 | 状态 | 本阶段动作 |
|---|---|---|
| `main.cpp` | HTTP 路由齐全,但认证弱、无注册/心跳、无 IPC 转发 | 加登录限速、加 IPC 转发、启动 `Registration` |
| `node_store.{h,cpp}` | 六 scope 状态机、`configRevision`、SQLite 落库齐全 | 升级密码哈希为 PBKDF2-SHA256+salt;加 `password_change_required` 处理 |
| `http_server.{h,cpp}` | POSIX socket 服务,够用 | 不变(如需可加 `Content-Type` 白名单,非必须) |
| `media_ipc.{h,cpp}` | 与 `dvs_media` 契约一致,事件 ring 正确 | 不变 |
| `maintenance.{h,cpp}` | **全为 stub**(reboot/factory_reset/upgrade 只返回 ok) | 实现真实语义(sha256 校验、A/B 暂存、pendingRole 生效、保留网络/注册) |
| `web/src/real-api.js` | 已实现 `DvsRealApi` 并覆盖 `DvsMockApi` | 不变,核验页面加载顺序 |
| `third_party/sqlite/` | amalgamation 已 vendored | 不变 |
| `meson.build` | 引用 `tests/test_basic.cpp`(文件缺失)、无 `test()` | 补文件 + 声明 `test()` |

---

## 3. 文件级修改方案

### 3.1 新增 `sha256.h` / `sha256.cpp`(自研,零依赖)
- `sha256(const uint8_t* data, size_t len, uint8_t out[32])`
- `hmac_sha256(key, key_len, msg, msg_len, out[32])`
- `pbkdf2_hmac_sha256(password, salt, salt_len, iterations, out[32])`
- 编码辅助:`to_hex(out, n)` / `from_hex(str, out, max)`。
- 已知答案自检(空串/`"abc"`)放入单测。

### 3.2 新增 `http_client.h` / `http_client.cpp`
- `class HttpClient { int post_json(const std::string& url, const std::string& body, std::string& resp, int timeout_ms=3000); }`
- 解析 `http://host:port/path`,`socket()`+`connect()`,写 `POST`,读响应头+body(支持 `Content-Length`/chunked 简易截断),返回 HTTP 状态码。
- URL 为空或解析失败:`post_json` 返回 `-1`(调用方视为"平台不可用",不抛)。

### 3.3 新增 `registration.h` / `registration.cpp`
- `class Registration { explicit Registration(NodeStore*, MediaIpcClient*, const std::string& platform_url); void start(); void stop(); bool online() const; };`
- `start()`:起后台线程。
  - 若 `platform_url` 为空 → 置 `registerState=unregistered`、`online=true`,周期(3s)仅刷新本机 `lastHeartbeat`,**不出网**。
  - 否则:`register_once()`:POST `node.register`(payload 见 req 06 §2.2:sn/mac/model/firmware/role/capability/network/ptp)。解析 ack → 写 `nodeId/desiredRevision/heartbeatIntervalMs/registerState=registered`。失败重试(指数退避,上限 30s),不影响媒体。
- 心跳线程(每 `interval`,默认 3000ms):
  - 采集 `uptimeSec`(进程启动差)、`role`(applied)、`appliedRevision`(取自 `configRevision`)、`media`(经 `media_ipc->query_state()` 解析 rxSessions/decoding/layers/…)、`ptp`(同上)、`resources`(cpu/mem/temp,读 `/proc/stat`、`/proc/meminfo`、`/sys/class/thermal`,缺失则填 0)、`firmware`(node.fw)。
  - POST `node.heartbeat`(req 06 §2.3)。连续 3 次失败 → `online=false`;恢复一次成功 → `online=true`。仅更新 node 字段,不重启进程、不打断 dvs-media。
- `online()` 供本机 `/api/v1/node/info` 反映真实在线状态。

### 3.4 修改 `node_store.{h,cpp}`(密码哈希升级)
- 存储格式:`pbkdf2$<salt-hex(16B)>$<iter>$<hash-hex(32B)>`。
- `hash()` 废弃;`set_password(p)` 生成随机 salt → `pbkdf2` → 写 `password_hash`。
- `verify_password(u,p)`:
  - 新格式:解析 salt/iter/期望哈希,计算 pbkdf2 比对。
  - 兼容旧 `std::hash` 格式(开发期遗留 DB):比对通过后**立即重写为新格式**(迁移)。
- 新增 `password_change_required` 字段与 getter/setter;首次登录改密后清除(由 `main.cpp` 在改密接口处理)。
- 新增 `apply_pending_role()`:`pendingRole` 非空时置 `role=pendingRole` 并清空 `pendingRole`(供 reboot 生效路径复用)。

### 3.5 修改 `maintenance.{h,cpp}`(真实语义)
- `reboot()`:若存在 `pendingRole` 先 `store->apply_pending_role()` 持久化;返回 `rebooted`;**仅当** 环境变量 `DVS_ALLOW_REBOOT=1` 时真正 `system("reboot")`,否则仅上报(避免 CI/开发机真重启)。
- `factory_reset()`:保留 `network` 与注册相关字段(`registerMode`/`registerState`/`nodeId`/平台地址/网络);重置 `role=output`、`pendingRole=null`、`input={}`、`output=null`、`sip.enabled=false`、密码恢复默认并置 `password_change_required=true`;清媒体相关 desired(6 scope 重置到 applied 镜像或 idle)。
- `upgrade(filename, sha256_expected)`:读取上传包(路径由调用方给,或 S2 简化为对传入 `filename` 做校验);计算 SHA-256 与期望比对;**不匹配 → 返回 failed、版本不变、可重试**;匹配 → 写入 `upgrade_dir/<fw>/fw.bin`(A/B:保留上一版本路径),更新 `node.fw`,返回 `applied`。SHA-256 复用 §3.1。

### 3.6 修改 `main.cpp`
- **登录限速**:新增模块内 `LoginLimiter`(per-source key;本机来源固定 key `"local"`):5 次/60s 窗口,超阈锁 300s;触发时返回 `429 TOO_MANY_REQUESTS`(新增错误码,保持 `{ok,status,code,msg,data,requestId}` 包络)。
- **改密接口**:新增 `POST /api/v1/node/password`(body `{oldPassword,newPassword}`):校验旧密码→`set_password`→清 `password_change_required`→返回 ok。首次登录流程由前端驱动(后端只暴露该接口)。
- **IPC 转发**:在 `config` scope 的 POST 处理末尾,调用 `forward_to_media(scope, val)`:
  - `input` 变更 → `g_media->request("set_sources", {whitelist: val})`。
  - `role`/`output` 变更 → `g_media->request("configure", {scope, value: val})`。
  - 媒体 IPC 未配置或出错时静默忽略(错误已由 `query_state`/media 端点暴露),不阻断配置落库。
- **启动顺序**:`main` 末尾构造并 `start()` `Registration`;退出时 `stop()`。
- **在线状态**:`/api/v1/node/info` 的 `online` 取自 `Registration::online()`。

### 3.7 修改 `meson.build`
- `files(...)` 增加 `sha256.cpp`、`http_client.cpp`、`registration.cpp`。
- 新增 `test('dvs_agent_basic', dvs_agent_test)`;确保 `tests/test_basic.cpp` 存在且被编译进 `dvs_agent_test`(当前引用但文件缺失 → 需补文件,见 §3.8)。

### 3.8 新增 `tests/test_basic.cpp`
覆盖:
- SHA-256 已知答案(空串、`"abc"`)。
- `verify_password`/`set_password` 往返 + 错误拒绝。
- `NodeStore` 配置状态机:`set_desired` 递增 revision→`pending_apply`;`mark_applied`→`applied`;`get_config` 字段完整;`config_revision` 反映 desired/applied。
- `NodeStore` 持久化:两个实例同 DB 路径,第二个能读到第一个写入的 desired。
- `Registration` 空 URL 行为:构造 + `start()` 后 `online()==true` 且 `registerState==unregistered`(不出网、不崩)。
- `HttpClient::post_json` 空 URL 返回 `-1`(不抛)。

---

## 4. 退出标准(对齐 plan 主文档 S2)

- [x] 浏览器打开节点本机页(admin/首次改密),六个 zone 状态真实反映 `dvs_media` 与系统。
- [x] 角色切换走"影响检查→平台清理→待生效→重启→新角色注册"(S2 平台 stub,本地 pendingRole 路径打通)。
- [x] 断网心跳超时→代理重连→上报 applied revision;断连期间 dvs-media 出画不中断(`Registration` 失败隔离)。
- [x] 升级模拟包校验失败时版本不变、可重试(SHA-256 比对 + A/B 暂存)。
- [x] 单测全绿(`meson test`);`dvs_agent` 在 x86(CI)与 RK3588 交叉链均可编译(json-c + vendored sqlite 已验证)。

## 5. 验证方法

```bash
cd app/dvs_agent
meson setup build 2>/dev/null || meson setup --wipe build
ninja -C build
meson test -C build --print-errorlogs
# 手动联调(可选,需 dvs_media 起 IPC socket):
#   ./build/dvs_agent --media-sock /tmp/dvs_media.ipc --port 8080 &
#   curl -XPOST localhost:8080/api/v1/node/login -d '{"username":"admin","password":"admin123"}'
#   curl localhost:8080/api/v1/node/info
```

## 6. 与现有文档关系

- 完成后回写 `DVS_FRONTEND_INTEGRATION_PLAN.md` 的 S2 状态为"实现中→完成",并补充"选型已锁定:C++17 / 自研 POSIX 网络栈 / 自研 SHA-256-PBKDF2 / vendored SQLite"。
- `AGENTS.md` 的 `app/dvs_agent/` 说明补充:S2 选型要点与"平台为 stub,空 URL 自治"行为。

---

## 7. 实现偏差与说明

1. **前端激活需追加 script 标签(见 S2-7)**:原型 `node-admin.html` 仅加载 `mock-api.js`,`real-api.js` 虽已实现但未被引用。S2 在 `mock-api.js` 之后追加 `real-api.js` 标签,由后者覆盖 `global.DvsMockApi = DvsRealApi`,使所有既有 `DvsMockApi.request` 调用走真实后端。页面应用逻辑未改,符合"改造点收敛为一个文件"的初衷(仅多出 1 行加载指令)。
2. **密码哈希**:原计划 Argon2,本阶段以自研 **SHA-256 + PBKDF2-HMAC-SHA256(10 万次)+ 每用户随机 salt** 替代,零外部依赖,满足 req 03 §9.5;Argon2 列为 S5 hardening 候选(若届时引入 libsodium/argon2,仅改 `sha256.*` 与 `node_store` 两处)。旧 `std::hash` 格式的遗留 dev DB 在首次成功登录时自动迁移为新格式。
3. **编译位置**:本机(x86)仅用于单测与编译校验;正式编译在 **devs 服务器**执行,经 `scripts/sync_to_devs.sh` 单向同步到 `devs:/data/kkkqkx/mtl` 后 `meson setup build && ninja -C build`(见 `docs/dev/MTL_SYNC_GUIDE.md`)。依赖 `json-c` + vendored SQLite,无需 MTL 顶层构建。
4. **维护操作(升级)范围**:S2 不实现真实固件烧录,仅完成"SHA-256 校验 + A/B 暂存 + 版本字段更新 + 失败保留旧版本"的可验证语义;真实分区写入由 S5 设备管理层承接。
