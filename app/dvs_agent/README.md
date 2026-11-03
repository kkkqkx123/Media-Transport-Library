# dvs_agent — DVS 节点代理

运行在每个节点（RK3588 盒子）上的代理（C++17）：提供本地 HTTP API 与静态管理前端，负责向平台注册/心跳、通过 IPC 驱动本机 `dvs_media` 引擎、执行维护操作（升级等），并以 SQLite 持久化节点状态。纯控制面，依赖仅 json-c + 内置 SQLite（来自 `dvs_common`，in-tree 编译），可本地构建：

```bash
cd app/dvs_agent && meson setup build && ninja -C build && meson test -C build
```

## 功能要点

- **注册/心跳**：向 `dvs_platform` 网关注册节点，周期心跳并上报容量自检（CPU/内存/负载/uptime，读 `/proc`，无外部依赖）；
- **媒体 IPC**：`media_ipc` 客户端连接本机 `dvs_media` 的 Unix socket，转发 configure/prepare/commit 等引擎控制请求；
- **维护操作**：`maintenance` 模块执行节点侧维护（如升级落地）；
- **本地 HTTP + Web 前端**：`web/` 下的 node-device-admin 静态页面，含登录限流（每来源 5 次/60 秒）。

## 结构

| 文件 | 说明 |
|---|---|
| `main.cpp` | 入口：装配各模块、HTTP 路由（API + 静态前端）、容量自检 |
| `node_store.cpp/.h` | SQLite 节点状态存储 |
| `registration.cpp/.h` | 平台注册与心跳 |
| `media_ipc.cpp/.h` | dvs_media IPC 客户端 |
| `maintenance.cpp/.h` | 维护操作 |
| `web/` | 静态前端（页面见 `web/src/pages/`，交付说明见 `web/src/开发交付说明.md`） |
| `tests/test_basic.cpp` | 基础 gtest-free 自测（`meson test` 运行） |
