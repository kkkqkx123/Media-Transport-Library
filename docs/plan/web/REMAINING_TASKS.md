# DVS Web 前端剩余任务清单

> 前端原型位于 `ref/video-control-platform/` 与 `ref/node-device-admin/`。  
> 后端真相源见 `docs/plan/BACKEND_STATUS_AND_REMAINING_TASKS.md`。  
> 本文只列前端需要后端支持的剩余任务，不重复后端内部任务。

## 1. 平台前端（video-control-platform）

| ID | 任务 | 依赖后端 | 退出标准 |
|---|---|---|---|
| WEB-PL-1 | 分组管理页面接入真实 API（删除校验、搜索、分页） | PL-1 | 操作返回统一包络 |
| WEB-PL-2 | 拼墙管理页面接入真实 API（新建、软删除、恢复） | PL-1 | 7 天恢复可用 |
| WEB-PL-3 | 端口绑定页面接入真实 API（拖拽绑定、离线 pending） | PL-1, ME-1 | 能力来自节点上报 |
| WEB-PL-4 | 传输配置页面接入真实 API（白名单、组播地址） | PL-1 | 地址冲突校验 |
| WEB-PL-5 | 图像调节页面接入真实 API（整墙下发） | PL-1 | 参数生效 |
| WEB-PL-6 | 高清底图页面接入真实 API（上传、拆分、下发） | PL-5 | 清屏后只剩底图 |
| WEB-PL-7 | 画面调度页面接入真实 API（场景、窗口、清屏） | PL-2, PL-7 | 同帧切换无撕裂 |
| WEB-PL-8 | 设备列表页面接入真实 API（退役、墓碑、批量升级） | PL-8 | 退役拦截重注册 |
| WEB-PL-9 | 播放任务板接入真实 API（八态、重试） | PL-8 | 状态流转正确 |

## 2. 节点本机前端（node-device-admin）

| ID | 任务 | 依赖后端 | 退出标准 |
|---|---|---|---|
| WEB-AG-1 | 登录/改密接入真实 API | AG-1 | 严格登录态 |
| WEB-AG-2 | 角色切换接入真实 API | AG-2 | 平台不可达只保存意图 |
| WEB-AG-3 | 输入口接入真实 API | ME-1 | 能力来自节点 |
| WEB-AG-4 | 输出设置接入真实 API | AG-3 | 超出能力拒绝 |
| WEB-AG-5 | 网络与注册接入真实 API | AG-4 | 断连自动回滚 |
| WEB-AG-6 | SIP 接入真实 API | AG-5 | SIP 不影响媒体 |
| WEB-AG-7 | 本机信息接入真实 API | AG-7 | 真实状态 |
| WEB-AG-8 | 维护操作接入真实 API | AG-6 | 升级失败保留旧版本 |
| WEB-AG-9 | 媒体状态/能力接入真实 API | ME-1/ME-2 | 不再返回空 |

## 3. 前端需要的后端缺口摘要

- 平台：业务校验、场景快照、prepare/commit、底图拆分、时钟资格、退役/墓碑/升级。
- 代理：登录态、角色切换、网络回滚、SIP、升级进度、媒体状态。
- 媒体：能力探测、query_state、src crop、音频路由、帧边界提交。

## 4. 验证

- 前端页面全部使用真实 API，不再使用 mock-api。
- 前端错误处理统一使用后端包络 `{ok,status,code,msg,data}`。
- 前端不自行计算布局拆分，只展示平台返回结果。
