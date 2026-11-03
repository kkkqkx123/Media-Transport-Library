# dvs_platform — DVS 平台服务

多节点媒体系统的平台侧控制服务（C++17，与 `dvs_agent` 同栈：json-c + 内置 SQLite，无其他运行时依赖）。负责节点注册/心跳网关、分组/墙/场景/绑定/白名单文档管理、布局像素拆分、跨节点 prepare/commit 屏障与回退、退役墓碑与串行批量升级。纯控制面，可本地构建：

```bash
cd app/dvs_platform && meson setup build && ninja -C build && meson test -C build
```

## 功能要点

- **节点网关**：接收各 `dvs_agent` 的注册/心跳（经 `http_client` 反向调用节点）；
- **文档 CRUD**：分组/墙/场景/绑定/白名单等以通用文档形式存储，挂载在 `/api/v1/<kind>[/<id>]`；
- **布局拆分**：`layout` 将整墙布局按各节点能力/位置拆成每节点的像素级子布局；
- **操作屏障**：`ops`（OpCoordinator）实现跨节点 prepare/commit 两阶段提交，失败按序回退；
- **升级**：串行批量升级节点代理，退役节点写墓碑防止复活；
- 统一 JSON 响应封装（`ok/status/code/msg/data/requestId`），支持 `Idempotency-Key` 幂等。

## 结构

| 文件 | 说明 |
|---|---|
| `main.cpp` | 入口：HTTP 路由（文档 CRUD、注册/心跳、操作编排）+ 静态前端 |
| `platform_store.cpp/.h` | SQLite 文档存储 |
| `layout.cpp/.h` | 布局像素拆分 |
| `ops.cpp/.h` | 跨节点操作屏障（prepare/commit/回退）、升级、墓碑 |
| `web/` | 静态管理前端（分组/墙/场景管理页面） |
| `tests/test_basic.cpp` | 基础自测（`meson test` 运行） |

复用 `dvs_common` 的 HTTP/SHA-256/SQLite 与 `dvs_agent` 的部分源码（in-tree 编译，不预抽公共库）。
