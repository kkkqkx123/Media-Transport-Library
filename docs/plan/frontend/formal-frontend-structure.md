# DVS 前端正式化：项目结构与服务端归属分析

## 1. 服务端归属

- **应由 C++ dvs-agent 作为唯一服务端**：`app/dvs_agent/main.cpp` 已提供 `/api/v1/*` API 与静态文件服务（`g_web_root` + `guess_ct`），并注入 Cookie 会话。
- **前端应为纯静态资源**：HTML/CSS/JS/品牌资源，不包含任何 Node 运行时或服务端逻辑。
- `web/src/server.mjs` 仅是原型期的 Node 静态服务器，与正式架构重复且会把 `server.mjs`/`package.json` 暴露在 Web 根目录。已移除，不再作为运行方式。

## 2. 当前结构仍属原型规范的点

1. **目录平铺/原型命名**：`packs/node-admin/`、`_shared/`、`vendor/`、`assets/brand/` 命名与路径（`../../_shared`）仍体现原型拼装方式；正式可按 `assets/`、`shared/`、`pages/`、`vendor/` 规整，但需同步 C++ 静态路径与 HTML 相对引用。
2. **品牌资源残留**：`assets/brand/ITC Design-logo-*.svg` 等第三方品牌文件仍存在，仅替换了文本/alt；正式应替换为 DVS 自有资源并删除 ITC 文件。
3. **开发期文件**：已移除 `server.mjs`/`package.json`；仍需确认 `vendor/` 仅包含正式需要的 TDesign 资源，避免未用大文件。
4. **缓存/安全头**：C++ 静态服务无 `Cache-Control`/CSP/压缩；正式发布前应在 `HttpServer`/`guess_ct` 补充或前置反向代理。
5. **多页面**：平台页与节点页混在 `packs/`；正式应明确入口与导航，节点页仅本机使用。
6. **测试/验收**：前端无自动化测试；建议对关键交互（登录、配置提交、维护）增加 E2E（Playwright）或保留 pytest 通过 RxTxApp 的间接覆盖。

## 3. 建议的后续改造

- 将 Web 根目录设为构建产物目录（如 `web/dist`），源码迁至 `web/src`，C++ `--web-root` 指向 `dist`。
- 引入轻量打包（Vite/Rollup）做资源指纹与压缩，但不改变“纯静态”定位。
- 补充 401/会话过期统一跳转与错误页。
- 清理所有 ITC 品牌文件与“演示/原型”文案。

## 4. 结论

正式前端 = 纯静态页面 + C++ dvs-agent 提供 API 与静态托管；Node `server.mjs` 不应参与运行时。
