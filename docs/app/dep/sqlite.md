# `app/dvs_common/third_party/sqlite` 使用与作用分析

## 1. 目录内容

`app/dvs_common/third_party/sqlite/` 仅包含两个文件：

- `sqlite3.c` — SQLite 的 amalgamation 单文件实现（本仓库版本为 SQLite 3.45.1）。
- `sqlite3.h` — 对应的公开 C API 头文件。

这是 SQLite 官方推荐的“amalgamation”分发形态：把整个核心库合并为一个翻译单元，编译时只需这两个文件，无需安装系统 SQLite 开发包或运行时库。

## 2. 作用

该目录为 DVS 控制面组件提供**内嵌、零外部依赖的持久化数据库能力**：

- 避免对目标板（RK3588）和构建主机的系统 SQLite 运行时/开发包依赖。
- 每个可执行文件（`dvs_agent`、`dvs_platform`）各自内嵌一份 SQLite 实现，二进制独立、可直接部署。
- 为控制面状态（节点身份/配置、平台文档/审计/墓碑）提供可靠的本地磁盘持久化，进程重启后状态可恢复。

SQLite 在本项目中只服务于**控制面**，不参与 MTL 数据面、编解码、显示等实时路径，因此不进入 RK3588 的实时调度链路。

## 3. 使用方式

### 3.1 编译集成（in-tree）

`dvs_common`、`dvs_agent`、`dvs_platform` 三个 meson 工程都把 `sqlite3.c` 直接编进自己的目标：

- `app/dvs_common/meson.build`
- `app/dvs_agent/meson.build`
- `app/dvs_platform/meson.build`

共同配置：

- include 路径包含 `third_party/sqlite`，以便 `#include <sqlite3.h>` 解析。
- 编译宏 `SQLITE_THREADSAFE=1`（多线程串行化模式）。
- 链接依赖：`threads`（pthread）、`dl`（`dlopen` 相关符号，按需）。
- C++ 侧额外定义 `_GNU_SOURCE`。

`dvs_common` 自身的 static_library 仅作语法检查目标；真正的消费方式是 `dvs_agent` / `dvs_platform` 在各自 meson 工程中直接编译这些源文件（in-tree），保证二进制独立。

### 3.2 运行时消费者

直接使用 SQLite C API 的代码只有两处：

1. `app/dvs_agent/node_store.h/.cpp` — `NodeStore`
   - 数据库表：`kv(k TEXT PRIMARY KEY, v TEXT NOT NULL)`。
   - 存储两类 JSON 文档：
     - `node`：节点身份、能力、网络、SIP、密码哈希、`configStates`、`pendingRole` 等。
     - `config`：6 个配置 scope（role/input/output/network/registration/sip）的状态机记录。
   - 启动时若表为空则写入种子数据（默认节点信息、默认密码 `admin123`、初始 config）。
   - 提供密码 PBKDF2-HMAC-SHA256 哈希的生成/校验（哈希算法在 `sha256.cpp`，SQLite 仅存字符串）。
   - 默认库文件：`/tmp/dvs_agent.db`，可通过 `--db` 覆盖。

2. `app/dvs_platform/platform_store.h/.cpp` — `PlatformStore`
   - 数据库表：
     - `docs(kind, id, data)`：通用 JSON 文档存储，kind 包括 node/group/wall/scene/binding/whitelist/playback。
     - `tombstones(id, expires_at)`：退役节点墓碑，30 天内拒绝重新注册。
     - `audit_log(seq, event)`：本地审计日志（自增 seq）。
   - 默认库文件：`/tmp/dvs_platform.db`，可通过 `--db` 覆盖。

### 3.3 并发与线程安全

- SQLite 以 `SQLITE_THREADSAFE=1` 编译，API 调用本身是线程安全的。
- 两处 store 另外各自持有 `std::mutex`（agent 的 `mtx_`、platform 的 `mu_`），在更高层串行化复合操作，避免读-改-写交错。
- 每个进程使用独立的 DB 文件，不跨进程共享同一库文件。

### 3.4 测试使用

- agent 单测（`app/dvs_agent/tests/test_basic.cpp`）使用 `/tmp/dvs_agent_test_*.db` 文件库，验证密码、状态机、持久化 reopen。
- platform 单测（`app/dvs_platform/tests/test_basic.cpp`）同时使用文件库与 `:memory:` 库，验证文档 CRUD、墓碑、审计等。

## 4. 设计特点与边界

- **无外部运行时依赖**：SQLite 代码完全静态编进二进制，部署时不需要 `libsqlite3.so`。
- **轻量 schema**：仅 `kv` / `docs` / `tombstones` / `audit_log` 四类表，全部数据以 JSON 文本存储，便于与 json-c 互操作。
- **无迁移机制**：schema 仅 `CREATE TABLE IF NOT EXISTS`；字段/结构演进需由应用自行兼容。
- **无加密/完整性校验**：数据库文件明文存储；安全边界由上层密码哈希与访问控制承担。
- **错误处理偏宽松**：例如 `PlatformStore` 构造未检查 `sqlite3_open` 返回值；`ensure_schema` 忽略 `sqlite3_exec` 错误详情。分析结论：当前实现假设本地文件可写，异常场景下可能静默失败。
- **编译体积/时间成本**：`sqlite3.c` 约 9MB，每次 clean build 需编译该翻译单元，但换来部署简单性。

## 5. 与其他模块的关系

- `dvs_media` 不直接使用本目录；它通过 IPC 与 `dvs_agent` 交互，持久化由 agent 负责。
- `rk3588_live_tx` / `rk3588_playout_rx` 不使用 SQLite。
- MTL 传输层与 SQLite 无耦合。

## 6. 相关文件索引

| 文件 | 说明 |
|---|---|
| `app/dvs_common/third_party/sqlite/sqlite3.c` | SQLite amalgamation 实现 |
| `app/dvs_common/third_party/sqlite/sqlite3.h` | SQLite 公开 API |
| `app/dvs_common/meson.build` | 共享编译/语法检查目标 |
| `app/dvs_agent/meson.build` | agent 编译集成 |
| `app/dvs_platform/meson.build` | platform 编译集成 |
| `app/dvs_agent/node_store.h/.cpp` | agent 持久化封装 |
| `app/dvs_platform/platform_store.h/.cpp` | platform 持久化封装 |
| `app/dvs_agent/tests/test_basic.cpp` | agent 持久化测试 |
| `app/dvs_platform/tests/test_basic.cpp` | platform 持久化测试 |
