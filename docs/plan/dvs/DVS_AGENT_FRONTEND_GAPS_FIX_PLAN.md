# dvs_agent 前端剩余缺口完整修改方案

> 对应映射文档：`docs/app/dvs_agent_frontend_mapping.md`。
> 对应代码：`app/dvs_agent`、`app/dvs_media`、`app/rk3588_live_ingest`、`app/rk3588_file_playout`。
> 本方案只描述最终目标与改造步骤，不保留历史过程。

## 1. 目标

使前端每个操作的状态诚实：已落库、意图已保存、引擎已生效、系统已执行四者不再混淆。做法是让代理状态机如实反映引擎结果，让前端只展示节点上报的真实能力，让高副作用系统动作默认安全且响应体如实标注。改造直接修改原模块，不新增替代模块，不保留旧兼容路径。

## 2. 对原缺口清单的复核结论

1. 角色保存不转发媒体不是缺口：延迟到重启激活是正确语义，保存时立即清空合成器反而会在会话中途断流。保留 pendingRole，激活点放在重启路径。
2. 输入口本轮只做到白名单意图，采集编码与 MTL 发送未被编排是事实，但 TX 编排接口未定义（采集口、编码参数、发流目标缺规格），不应与前端缺口同批实施，延期。
3. 输出标记已生效但未按引擎结果更新是真缺口，需要改为 pending 加后台按结果翻转，并加 revision 守卫。
4. 网络默认意图落库是正确的安全默认，真缺口是 `applied` 被误读为系统已切换；本轮只澄清语义并让响应诚实，不落地真实系统应用。
5. 维护默认门控是正确的安全默认，真缺口是响应体不区分受控与真实执行；不应反转门控为默认执行。
6. SIP 不是纯占位，后端已有参数校验加 TCP 可达性探测；本轮保留该探测语义并明确为连通性探测，不做完整会话，不移除 scope。
7. 能力探测默认值与环境覆盖是允许的降级，真缺口是空数组被当作未上报而保留种子导致掩盖零端口；应区分不可达保留与零端口传播。
8. 前端用 profile 回退端口是真缺口，应改为只用上报，profile 只留作规格说明。

## 3. 总体改造顺序

1. 先做输出状态反馈与能力前后端诚实化，两条链路接近完成且无系统副作用。
2. 再做输入白名单失败显性化与维护响应诚实化，只改状态表达，不改系统副作用。
3. 网络真实应用、SIP 完整会话、输入采集 TX 编排全部延期，只在本轮澄清语义与文档。

## 4. 工作包

### WP1 角色延迟激活（维持现状并澄清）

涉及文件：`app/dvs_agent/main.cpp`、`app/dvs_agent/maintenance.cpp`、`app/dvs_media/engine/engine_ctx.c`。

- 保存路径继续只写 pendingRole，不立即转发媒体清空合成器；立即转发会中断当前会话，违背待重启语义。
- 激活点继续放在重启路径：提升 pendingRole 后若新角色为 input 则调用媒体 `clear`。
- 前端继续显示 pendingRole 待重启。
- 本轮不新增保存时转发，只补注释与文档说明。

完成标准：保存后媒体保持旧角色运行，重启后 role 与 pendingRole 收敛；文档不再把延迟激活记为缺口。

### WP2 媒体执行结果反馈（带 revision 守卫）

涉及文件：`app/dvs_agent/main.cpp`。

- input 保持同步转发：引擎拒绝时用引擎返回的错误覆盖为 `mark_failed`，不再只记日志。
- output 改为 pending 加后台：保存后不预先 `mark_applied`，HTTP 立即返回 pending，后台线程按引擎 `configure` 结果翻转；后台提交前重读 desiredRevision，若已被更新则丢弃旧结果，避免覆盖新保存。
- 前端继续轮询到 applied/failed 为准。

完成标准：引擎失败时状态为 failed 并保留原因；并发保存时旧结果不覆盖新 pending。

### WP3 能力探测与前端展示（区分不可达与零端口）

涉及文件：`app/dvs_agent/main.cpp`、`app/dvs_agent/web/src/_shared/hardware-capabilities.js`。

- 代理启动与请求时合并引擎能力：引擎不可达时保留已有值，引擎明确报告空数组时如实传播空数组，不再跳过空数组。
- PTP 与 MTL 版本继续在每次 capability 请求时按引擎实时状态刷新。
- 输入口继续用 `/dev/video*` 枚举，输出口继续用 DRM connector；本轮不新增总线信息字段。
- 前端 `resolve()` 只用上报端口；无上报或零端口时显示未上报并禁用端口操作；profile 只留作规格与 safeModes 参考。
- 前端渲染加空安全守卫，避免空端口数组导致选中逻辑崩溃。

完成标准：上报零端口时前端显示零端口而不是伪端口；引擎不可达时保留旧值。

### WP4 网络系统应用（本轮只澄清语义，不落地执行）

涉及文件：`app/dvs_agent/main.cpp`、文档。

- 不拆分新模块，不新增 netplan/systemd/iproute2 执行；`apply_system_network` 保留原位，默认仍为意图持久化。
- 明确 `applied` 表示意图已落库，不代表系统网络已切换；响应与文档如实说明。
- 快照与回滚逻辑保留；真实系统应用延期，需另立规格与隔离环境。

完成标准：文档与注释不再把意图落库误述为系统已切换；无新增系统副作用。

### WP5 维护系统动作（保留安全默认，只让响应诚实）

涉及文件：`app/dvs_agent/maintenance.cpp`、`app/dvs_agent/main.cpp`。

- 不反转门控：重启与升级切换继续默认关闭，生产通过显式环境开启。
- 重启响应如实标注是否受控；受控时不谎称已重启，不把 lifecycle 置为 rebooting。
- 恢复出厂保持网络与注册，清空媒体并重置密码的逻辑不变。
- 升级继续区分 staged 与 applied，校验失败保留旧版本。

完成标准：开发机调用维护接口不会误重启或误切换；响应体可区分受控与真实执行。

### WP6 SIP 范围决策（保留连通性探测，不移除 scope）

涉及文件：`app/dvs_agent/main.cpp`、前端 `node-admin.js` 与说明文档。

- 不移除 scope，不改 `SCOPES` 与种子结构，避免不必要的库迁移。
- 保留现有语义：关闭直接落库，启用做参数校验加 TCP 可达性探测，失败明确 `mark_failed`，不影响媒体。
- 文档与前端明确标注为连通性探测，完整会话延期。

完成标准：SIP 启用失败时状态为 failed 且原因明确；前端不承诺完整会话。

### WP7 输入采集与 MTL TX 编排（延期）

- 本轮不扩展 `dvs_media` 为双模式，不复用 live_ingest 采集链，不扩展 IPC 输入与 TX 状态。
- 输入 scope 本轮语义冻结为白名单意图加端口校验；文档明确 `dvs_media` 为 RX-only。
- TX 编排需先补采集口、编码参数、发流目标与状态模型规格，再另立任务。

完成标准：文档明确延期，前端不承诺采集受控；本轮无引擎改动。

### WP8 前端能力基线清理（只用上报，加空安全）

涉及文件：`app/dvs_agent/web/src/_shared/hardware-capabilities.js`、`app/dvs_agent/web/src/pages/node-admin/node-admin.js`。

- `resolve()` 只用上报端口，无上报或零端口时返回空数组并标记未上报。
- safeModes 与自定义范围按已选端口动态计算；无端口时不伪造选项，禁用保存。
- profile 只保留规格说明用途。

完成标准：无上报时页面显示未上报；有上报时选项与上报一致。

## 5. 文件级变更清单（本轮实际落地）

- `app/dvs_agent/main.cpp`：输出改为 pending 加后台 revision 守卫，输入引擎拒绝时置 failed，能力合并区分不可达与零端口。
- `app/dvs_agent/maintenance.cpp`：重启响应如实标注受控与否，受控时不谎称 rebooting。
- `app/dvs_agent/web/src/_shared/hardware-capabilities.js`：只用上报端口，加未上报标记与空安全。
- `app/dvs_agent/web/src/pages/node-admin/node-admin.js`：空端口禁用保存，输出重协商走 pending 轮询，SIP 标注为连通性探测。
- `app/dvs_media`、`app/rk3588_live_ingest`：本轮无改动，TX 编排与采集能力延期。

## 6. 验证方案

- 代理单元测试：配置状态机、白名单校验、网络回滚、维护返回值。
- 媒体 IPC 测试：configure/prepare/commit/clear/query_state/capability。
- 集成测试：角色切换后重启提升；输出配置变更后合成器重建；输入白名单拒绝未授权源。
- 前端检查：无 capabilities 时显示未上报；端口列表与节点上报一致。
- 网络与维护测试仅在隔离环境执行，避免误重启当前开发机。

## 7. 风险与约束

- 输出后台线程必须带 revision 守卫，否则旧结果会覆盖新保存的 pending。
- 网络与维护保持安全默认，不新增系统副作用；生产开启需显式环境并在隔离环境验证。
- 能力零端口必须如实传播，前端必须空安全，不得回退伪端口。
- 输入 TX 编排延期，期间不得把白名单意图误述为采集受控。
- 所有改造直接修改原模块，不创建替代模块或兼容层；注释只描述意图，不引用文档编号。
