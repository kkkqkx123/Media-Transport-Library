# dvs_common — DVS 控制面共享基础设施

`dvs_agent` 与 `dvs_platform` 共用的 C++17 基础组件（HTTP 服务/客户端、SHA-256、内置 SQLite amalgamation），以及两个应用的静态 Web 前端共享资源（`web/`）。纯控制面，无 MTL/Rockchip 依赖，可本地构建验证。

## 结构

```
include/
  http_server.h   # 轻量 HTTP 服务（含 WS 帧编解码与同端口 Upgrade 接管）
  http_client.h   # HTTP 客户端（含持久化 WsClient 拨号）
  sha256.h        # SHA-256（登录口令摘要等）
  dvs_contract.h  # 平台—代理共享契约：信封、消息标识、鉴权入口、六类消息校验、传输模式缺省
src/              # 对应实现（含 dvs_contract.cpp 校验表）
third_party/sqlite/
  sqlite3.c/.h    # SQLite amalgamation（单文件内置，无外部运行时依赖）
web/
  _shared/        # 前端共享 JS/CSS（能力上报、节点管理、主题等）
  assets/brand/   # 品牌资源
```

## 消费方式

不作为链接库分发：`dvs_agent` 与 `dvs_platform` 的 meson 工程直接把这些源文件编进自己的可执行文件（in-tree 编译），保证两个二进制各自独立、无新增运行时依赖。本目录自身的 static_library 仅作语法检查目标。
