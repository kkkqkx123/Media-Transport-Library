# DVS Platform / Agent / Media 通信与分离分析

## 1. Platform 与 Agent 的通信

- 协议：HTTP REST over TCP。
- Agent -> Platform：
  - `POST /api/v1/nodes/register`：注册节点，上报 SN/MAC/型号/固件/角色/agentBase/几何尺寸/能力/网络/PTP。
  - `POST /api/v1/nodes/heartbeat`：周期心跳，上报运行状态与 applied revision。
- Platform -> Agent：
  - 通过注册时上报的 `agentBase` 反向调用节点 HTTP API。
  - 典型调用：配置同步、`/api/v1/node/media/layout` 下发布局、升级。
- Platform 负责全局编排：节点文档、布局像素拆分、跨节点 prepare/commit 屏障、回退、批量升级。

## 2. Agent 与 Media 的通信

- 协议：Unix domain socket 上的 JSON 行协议。
- Agent 是客户端（`MediaIpcClient`），Media 是服务端（`ipc_server`）。
- 通信内容：`configure`、`prepare`、`commit`、`abort`、`clear`、`query_state`、`hello` 等。
- 特性：同步请求/响应、messageId 匹配、超时与重连、事件行缓存、版本握手。
- Media 负责数据面：MTL RX、MPP 解码、DRM/ALSA、布局执行、能力探测。

## 3. 是否有必要分离

有必要，且当前边界基本正确。

- Platform 与 Agent 分离：Platform 是中心编排服务，Agent 是节点本地控制面；通过 HTTP 支持远程节点和全局操作。
- Agent 与 Media 分离：Media 是数据面，必须独立进程运行，避免控制面阻塞实时媒体链路；Unix socket 本地通信延迟低且隔离清晰。
- Bundle 边界：Agent 与 Media 作为 `dvs-node` 同包发布，保证 IPC 版本和运行时配对；Platform 可独立升级。
- 不应把 Media 合并进 Agent，也不应让 Platform 直接控制 Media，否则会破坏数据面实时性、节点配置一致性与安全边界。

## 4. 建议

- 保持 HTTP 作为 Platform-Agent 通道，Unix socket 作为 Agent-Media 通道。
- 保持 Bundle 版本与 IPC 版本作为配对契约。
- 继续通过 Agent 转发所有媒体控制请求，不让 Platform 绕过 Agent 直接操作 Media。
- 在需要扩展时，优先扩展消息契约与超时/重试语义，而不是合并通信通道。
