# DVS IPC 异步化改造方案(改进项 1–2 细化)

> 来源:`docs/app/dvs_node_analysis.md` §4.3 的实施顺序第 1、2 步。
> 范围:① `app/dvs_media/ipc`(ipc_server 长连接化 + messageId 幂等 + prepare 异步 + 事件推送 + revision 校验);② `app/dvs_agent/media_ipc`(配套客户端升级)与 `main.cpp` 接入。
> 原则:**保留 UDS + JSON 行传输不换**;引擎四模块(session_mgr/layout_executor/…)仍为 stub,本方案只打通"协议与状态层",为 S1 功能填充预留接口。

## 1. 目标与非目标

### 目标

| # | 改进 | 对应需求差距 |
| --- | --- | --- |
| G1 | ipc_server 从"每请求一连接"改为长连接会话,支持一连接多请求 | 事件推送、并发请求的前提 |
| G2 | 请求/响应携带 `messageId`,响应回带同 id;重复 messageId 幂等返回首次结果 | req 06 §2.1 |
| G3 | `prepare` 异步执行:立即回 `accepted`,引擎侧完成后以 `media.prepare.result` 事件回推同一连接 | req 04 §6.2(等待解码首帧不能阻塞其它消息) |
| G4 | 事件反向推送:media → agent 订阅式推送(`media.prepare.result`、`media.layout_applied`、`media.state_changed`) | req 06 §2.7 |
| G5 | revision/operation 校验:旧 revision 拒绝(`STALE_REVISION`)、同 revision 幂等、未 prepare 先 commit 拒绝(`INVALID_STATE`)、缺 operationId 拒绝 | req 03 §12.2、req 04 §6.4 |
| G6 | agent 客户端:持久连接 + 自动重连、requestId 生成、poll 超时、后台事件线程、引擎不可用时返回结构化错误码 | 错误码表 req 06 §4 |
| G7 | HTTP 层上抛:`/api/v1/node/media/*` 在引擎不可达时返回 `MEDIA_ENGINE_UNREACHABLE`,新增 `/api/v1/node/media/events` 拉取缓存事件 | 本机页不再把故障显示为空状态 |

### 非目标

- 不实现真实会话/布局/音频(引擎四模块仍 stub,prepare 的"异步工作"当前为校验 + 短延时模拟)。
- 不引入 protobuf、不换传输方式。
- 不实现平台 WSS 通道(改进项 3,另行方案)。
- 不引入新第三方依赖(仅 json-c / pthread / poll)。

## 2. dvs_media 侧设计

### 2.1 状态机扩展(`ipc/state_machine.{h,c}`)

```c
typedef enum {
  DVS_OP_IDLE = 0,
  DVS_OP_PREPARING,     /* prepare 已受理,结果未回推 */
  DVS_OP_PREPARED,
} dvs_op_state_t;

typedef struct {
  int  revision;              /* 已生效 revision */
  char state[16];             /* idle/prepared/committed(沿用旧字段,兼容 query_state) */
  char operation_id[64];      /* 当前/最近一次 operation */
  dvs_op_state_t op_state;
  int  prepared_revision;     /* prepare 通过时的 revision */
} dvs_state_t;
```

新增接口(线程安全,内部互斥):

```c
/* 返回 0=接受, 负数=错误码(DVS_ERR_STALE_REVISION / DVS_ERR_INVALID_STATE / DVS_ERR_INVALID_ARG) */
int  dvs_state_begin_prepare(const char* opid, int revision, char* err, size_t errlen);
int  dvs_state_finish_prepare(const char* opid);            /* prepare 成功落 prepared */
void dvs_state_fail_prepare(const char* opid);              /* 回 idle */
int  dvs_state_commit(const char* opid, int revision, int* applied_rev, char* err, size_t errlen);
void dvs_state_abort(const char* opid);
void dvs_state_clear(void);
void dvs_state_snapshot(dvs_state_t* out);                  /* query_state 用 */
```

校验规则(评审定稿,见 §5 开放问题):

| 规则 | 判定 | 错误 |
| --- | --- | --- |
| 幂等 | prepare/commit 的 `operationId` 与当前 operation 相同且 revision 相同 → 返回上次结果 | 无(幂等成功) |
| 旧 revision | `revision < state.revision` | `STALE_REVISION` |
| 状态迁移 | commit 时 op_state ≠ PREPARED(含重复 commit 同一 operation) | `INVALID_STATE` |
| 参数 | 缺 `operationId` | `INVALID_ARGUMENT` |
| revision 缺省 | 请求不带 `revision` 时取 `state.revision + 1` | — |

### 2.2 ipc_server 重写(`ipc/ipc_server.c`)

连接模型:

```text
accept 循环(主线程)
  └─ 每个客户端连接一个 reader(线程,或主线程顺序处理首个连接 + 后台线程池;V1 取"每连接一线程",客户端只有 agent 一个)
       ├─ 逐行读取请求
       ├─ 同步消息:立即回写(带原 requestId)
       └─ 异步消息(prepare):回 {"status":202,"msg":"accepted"} → 入 worker → 完成后向该连接回写事件行
事件广播:全局注册表 {fd, mutex};commit 成功后向所有连接广播 media.layout_applied / media.state_changed
写保护:每连接一把 write mutex(reader 回响应与 worker 回事件共用)
```

消息语义定稿:

| 消息 | 同步/异步 | 行为 | 响应 |
| --- | --- | --- | --- |
| `configure` | 同步 | 记录 role/input/output 到状态机(暂存 JSON,不生效) | ok |
| `prepare` | **异步** | 校验(§2.1)→ 回 accepted → worker 模拟准备(当前 stub:校验 payload.layout/sources 存在, usleep 后 finish) → 事件 `media.prepare.result{status:ready}` | 202 + 事件 |
| `commit` | 同步 | 校验 revision/operation → 状态机 commit → 广播 `media.layout_applied` | ok + appliedRevision |
| `abort` | 同步 | 仅 PREPARING/PREPARED 可中止,否则 INVALID_STATE | ok / 错误 |
| `clear` | 同步 | 幂等,状态回 idle | ok |
| `set_sources` | 同步 | 暂存白名单 JSON | ok |
| `query_state` | 同步 | 返回真实状态机快照(revision/state/operationId/opState) | ok |
| `event` | 同步 | 预留(客户端订阅确认) | ok |

响应 envelope 保持不变,`requestId` 回带请求的 `messageId`(缺省 `req-<seq>`)。错误码:`STALE_REVISION(409)`、`INVALID_STATE(409)`、`INVALID_ARGUMENT(400)`。

### 2.3 头文件与 API

```c
/* ipc_server.h */
typedef struct dvs_ipc_server dvs_ipc_server;
/* 启动服务(阻塞);事件回调可选,供单测注入 worker 模拟时长 */
int  dvs_ipc_server_run(const char* sock_path);
void dvs_ipc_set_prepare_delay_ms(int ms);   /* 默认 50;单测可设 0 */
```

## 3. dvs_agent 侧设计

### 3.1 MediaIpcClient 重写(`media_ipc.{h,cpp}`)

```cpp
class MediaIpcClient {
public:
    MediaIpcClient(const std::string& sock_path);
    ~MediaIpcClient();
    /* 同步请求:自动生成 requestId,connect-on-demand + 一次重连重试,poll 超时(默认 2000ms) */
    std::string request(const std::string& type, const json_object* payload,
                        int* http_like_status /*可空*/);
    std::string query_state();                 /* 兼容旧调用 */
    /* 事件:后台线程常驻读,事件行入环形缓存(上限 64,新的挤掉旧的) */
    json_object* drain_events();               /* 取走全部缓存事件(数组) */
    bool connected() const;
private:
    void ensure_conn();                        /* 断线重连 */
    void reader_loop();                        /* 区分响应 vs 事件(type 前缀) */
    ...
};
```

要点:

- **响应/事件区分**:一行 JSON 含 `requestId` 且非 `media.*` 事件类型 → 响应;`type` 为 `media.prepare.result` / `media.layout_applied` / `media.state_changed` → 事件缓存。
- **超时**:`poll(fd, POLLIN, timeout_ms)`,超时返回 `{"ok":false,"code":"MEDIA_ENGINE_TIMEOUT"}` 并断开重连。
- **引擎不可达**:`connect` 失败返回 `{"ok":false,"status":503,"code":"MEDIA_ENGINE_UNREACHABLE","msg":"..."}`。
- 线程安全:请求串行(一把请求锁),事件线程独立。

### 3.2 main.cpp 接入

| 端点 | 变更 |
| --- | --- |
| `/api/v1/node/media/status` | 走 `request("query_state")`;引擎不可达/超时时**原样透传错误 envelope**(不再是空对象) |
| `/api/v1/node/media/events`(新增 GET) | 返回 `drain_events()` 数组 |
| `/api/v1/node/media/prepare`(新增 POST) | body 透传为 prepare payload,返回 202 accepted(事件稍后经 events 端点可见) |

## 4. 测试与验证

| 层 | 内容 | 位置 |
| --- | --- | --- |
| dvs_media 单测(新增,`tests/meson.build` 注册 meson test) | 状态机:revision 幂等/STALE/INVALID_STATE 迁移;ipc_server:mock 客户端走 prepare 异步→事件回推、重复 messageId 幂等、clear 幂等 | `app/dvs_media/tests/test_state_machine.c`、`test_ipc.c` |
| dvs_agent 单测(扩展) | MediaIpcClient 对不存在的 sock 路径返回 MEDIA_ENGINE_UNREACHABLE envelope(不崩溃);requestId 生成 | `app/dvs_agent/tests/test_basic.cpp` |
| 手工联调 | 双进程:起 dvs_media --ipc-sock,用 tests/mock_client.py(新增)依次 configure→prepare(收 accepted 与事件)→commit(收 appliedRevision 与广播)→query_state | `app/dvs_media/tests/mock_client.py` |

编译验证约束:dvs_media 需要 MTL/MPP/FFmpeg(仅 RK3588/装了依赖的环境),其 `ipc/` 与 `engine/` 源文件只依赖 json-c + pthread,**用独立 gcc 命令对 ipc/*.c 做语法/链接级检查**即可覆盖本次改动;tests 用纯 C 编译运行(不链接 mtl)。dvs_agent 依赖仅 json-c + sqlite,可完整 `meson setup && ninja` 验证。

## 5. 开放问题(评审项)

1. prepare worker 的真实化顺序:先接 layout_executor 校验,还是先接 session_mgr?建议后者(先有会话才能预解码)。
2. 事件缓存是否需要按 `operationId` 去重/合并——V1 先按"新的挤掉旧的"处理。
3. `configure` 暂存的 JSON 何时生效(等 S1 引擎接入后再定义语义)。
