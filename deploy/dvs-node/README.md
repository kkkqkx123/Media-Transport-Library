# dvs_node — DVS 节点发布包

`dvs_agent`（控制面）与 `dvs_media`（媒体引擎）仍是两个进程，
但作为同一个包同版本发布、同启停、同升级。本目录承载与代码解耦的
发布运维面，不引入新的运行时依赖。

## 内容

| 文件 | 说明 |
|---|---|
| `VERSION` | 包版本（当前为 1），两二进制的 `--version` 与契约头必须与此一致 |
| `dvs-node.sh` | 单机编排脚本：先起受控模式媒体引擎，再起代理，共用同一 IPC socket |
| `dvs-media.service` / `dvs-agent.service` | systemd 单元，声明双进程的启停顺序与重启策略 |
| 本说明 | 包级运维文档 |

## 版本契约

单一契约源为 `../../app/dvs_common/include/dvs_node_bundle.h`，
两进程引用同一份包版本与 IPC 版本定义。
引擎侧 `hello` 握手上报版本与状态快照，代理在启动时核对、
并经 `GET /api/v1/node/version` 对外呈现，供运维确认同包配对。
配置下发统一走单一配置谓词。

## 部署

单机联调可用 `dvs-node.sh`（需经 `DVS_MEDIA_CONFIG` 给出媒体静态基
础配置）；生产节点使用两个 service 单元。升级时同时替换两个二进制，
任一 `--version` 与 `VERSION` 不符即拒绝拉起。
