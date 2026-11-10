# DVS 节点包（dvs_media + dvs_agent）板端部署与验证设计

适用对象：`app/dvs_media`（节点媒体引擎，受控/独立双模式）与 `app/dvs_agent`（节点代理，本地 HTTP + 静态前端）在 RK3588 开发板上的部署与验证。本文只定分层、拓扑、取证口径与判定，不改生产代码。裸链路基线与显示判定口径仍见本目录既有 design 文档，本文承接其上，只补“受控包”新增的版本契约、IPC、六 scope 状态机与 Web 后端闭环。

相关资产：节点包说明见 `deploy/dvs-node/README.md`，编排脚本为 `deploy/dvs-node/dvs-node.sh`，systemd 单元为同目录两 service 文件，版本契约为 `app/dvs_common/include/dvs_node_bundle.h` 与 `deploy/dvs-node/VERSION`。裸管线跑法与门禁见 `rk3588-manual-verification-runbook.md` 与 `manual-scripts-inventory.md`，同步规范见 `docs/dev/board-mtl-sync.md`，正式库位置见 `docs/dev/RK3588_DEPLOYMENT.md`。后端真相源见 `docs/plan/dvs/BACKEND_STATUS_AND_REMAINING_TASKS.md`。

## 1 目标与成功标准

回答三个问题：包能否同版本拉起；受控链路能否经 IPC 与 Web 后端驱动出画出声；异常输入是否被诚实拒绝而不谎报运行。

成功标准为同组输入可重复：两二进制 `--version` 与包版本一致；`hello` 握手配对；无引擎时各查询面返回结构化失败而非空串；有引擎时 `query_state` 的 lane 与 TX 快照为真引擎值；Web 后端六 scope 按 `pending_apply/applied/failed` 流转且旧 revision 不覆盖新值；显示器有人眼见证结论；失败能定位到打包、IPC、代理、引擎、环境五段中的一段。

## 2 部署对象与版本契约

节点包为同包同启停同升级的两个进程：`dvs_media` 先起（受控模式，握住 IPC socket），`dvs_agent` 后起（连同一 socket，启动即 `hello` 核对）。契约只有两数：包版本（当前为 1）与 IPC 代际（当前为 1），单一来源为 `dvs_node_bundle.h`。运维面以 `VERSION` 文件为准，任一 `--version` 与之不符即拒绝拉起。引擎 `hello` 回执携带版本与状态快照，代理在启动时记一次 verdict 并经 `GET /api/v1/node/version` 对外呈现 `match/gateBlocked/gateReason/engineLink`，供运维确认同包配对。配置下发统一走单一 `configure` 谓词，不另设旁路。

## 3 构建

`dvs_media` 必须交叉编译：依赖 MTL、Rockchip MPP（必需）、librga（必需）、libdrm、ALSA、json-c、FFmpeg。复用 `rk3588_playout_rx` 的 `rx/` 与 `shared/`（含探针与解码辅助）源码与 `rk3588_live_tx` 的 donor chain（采集/编码/发送链路），meson in-tree 编译，不复制。本地或编译服务器按 `docs/dev/compile/rk3588-apps-build.md` 用 aarch64 sysroot 与交叉文件构建，产物为 `app/dvs_media/build/dvs_media`（板上为 aarch64，不可在 x86 直接运行；其单元测试亦为 aarch64 产物，只能上板跑）。

`dvs_agent` 为纯控制面（json-c + 内置 SQLite），可在本机直接构建验证：`meson setup build && ninja -C build && meson test -C build`。其 `dvs_agent_test` 为 x86 可跑的自测，覆盖密码、状态机持久化、注册本地模式、契约与 IPC 失败包络。`dvs_platform` 同栈，验证时仅作对端（注册/心跳网关），本设计不将其作为板内部署项。

构建一致性门禁为双端 `md5sum` 对齐：仓库永远是源，板上永不改源码，二进制重编后推送再抽查。板上只允许 `gcc` 直编探针类单文件，不编译正式工程。

## 4 板端部署位置与方式

正式位置与调试位置不混用。正式部署沿用 `RK3588_DEPLOYMENT.md` 的系统库目录（MTL/DPDK/XDP/BPF 进 `/usr/lib/aarch64-linux-gnu`）与应用隔离目录；节点包两个二进制进 `/usr/bin`（service 单元即按此路径写死），静态前端进 `/usr/share/dvs/web`，DB 进 `/var/lib/dvs/agent.db`，IPC socket 为 `/run/dvs/media.sock`，环境覆盖经 `/etc/dvs/node.env`。调试位置沿用 `~/code/mtl` 五目录规范（`bin/` 运行态、`src/` 只读镜像、`perf/streams/` 数据态），`dvs_media/dvs_agent` 调试二进制放 `bin/`，静态前端用仓库 `app/dvs_agent/web/src` 直接指认，不复制。

单机联调用 `dvs-node.sh`（需经 `DVS_MEDIA_CONFIG` 给出媒体静态基础配置，共用同一 IPC socket，先起引擎等 socket 就绪再起代理）。生产用两个 service 单元，声明启停顺序与重启策略（media 先行，agent 依赖 media）。升级时同时替换两个二进制，任一版本不符即拒绝拉起，不做混搭运行。

静态基础配置复用 file_playout 配置语义：网络双口与收发地址、宽高帧率、视频与音频端口、pacing 档（默认窄间隔）、播放延迟、DRM 设备与连接器、ALSA 设备、CPU 与 MTL 工作核列表、PTP 模式。受控模式下该文件只给网络与基线参数，布局经 IPC 到达；独立模式下该文件必须带至少一路流，否则直接拒绝。端口前缀支持 `kernel:` 与 `native_af_xdp:`。AF_XDP 是否可用是网卡驱动/XDP 程序支持决定的硬件约束：本板当前驱动组合不支持（dispatcher 对象加载失败且缺 `xsk_def_xdp_prog.o`），这不记为问题，本板统一用 `kernel:` 后端（与 `run_tx_rx_e2e.sh` 默认一致）。双口初始化要求两 NIC 都有 IP，无 IP 的 NIC 先临时配测后删。

## 5 前置门禁

沿用 `board_env.sh status` 作统一门禁：DRM 空闲、HDMI 连接、采集设备空闲且有时序锁存、声卡空闲且无 PulseAudio 竞争、网卡有 IPv4、测试 UDP 端口空闲。任一 `BLOCKED` 即为动作，不猜测。`run --` 语义为准备跑恢复，`Ctrl-C` 也恢复桌面。MTL 前置为 hugepages 与 MtlManager，缺失时建会话失败记环境失败。PTP 域只在绝对时延轮次要求同域，相对锚定轮次只看节奏。

新增的包级门禁由后文调试脚本承接：socket 存在、双 `--version` 同包、`hello` 配对、代理 `version/info/capability` 可读。门禁不过不跑流。

## 6 分层验证

### V0 打包与版本

目的为确认推上板的是同一包。方法为板上分别执行两二进制 `--version` 并与 `VERSION` 比对；再调代理 `GET /api/v1/node/version` 看 `bundleVersion/ipcVersion/match/engineLink`。无引擎时 `match=false`、`engineLink.state=offline` 为预期诚实值，不记失败；有引擎时必须 `match=true`。任一版本串不符即停，查推送与 `md5sum` 对齐。

### V1 IPC 通道

目的为确认引擎服务与协议代际。方法为经 Unix socket 发 `hello`、`capability`、`query_state` 三个只读动词。`hello` 看版本与 revision 快照；`capability` 看输出口、输入口、解码上限、图层数、TX 上限、PTP 与 MTL 串；`query_state` 看 `state/revision/lanes/audio/display/rxSessions/decoding/layers/ptp/tx` 全字段存在，空源时 lanes 为空数组而非缺字段。传输失败必须为结构化包络（未配置、不可达、超时三态），不允许空串。`prepare/commit/abort/clear` 的状态机语义（幂等重放、stale 拒绝、并发拒绝、引擎失败不记已提交）由 x86 侧单测覆盖，板上只做一次正常 `prepare/commit` 与一次错参 `prepare` 反例。

### V2 代理 HTTP（Web 后端）

目的为确认控制面在无屏环境可用，这是 Web 前端的所有真源。本层全部用 `curl` 打代理本地端口，无需浏览器。只读组为 `info/role/capability/capacity/version/media/status/media/lanes/media/tx/media/sources/media/logs/media/events/config/<scope>` 查询、`audit-events`、`nodes` 本地投影；写入组为 `login/password/logout` 与六 scope 的 `config/<scope>` 保存。破坏性动作为 `reboot/factory-reset/upgrade`，验证中不执行，只核对接口形状与审计落盘；`upgrade/upload` 如需跑只传小文件到 `/tmp` 随即删除。`media/prepare/commit/abort/layout` 为平台令牌通道，本地验证用查询面代替，不伪造平台下发。

通过标准为每个动词的包络都带 `ok/status/code/msg/data/requestId`；无引擎时媒体查询面诚实报 `MEDIA_ENGINE_UNCONFIGURED` 或 `engineUnreachable`，`lanes` 为空数组，`logs` 带 `engineLink` 与 `capStale`；有引擎时 `media/status` 的 `lanes/tx/ptp` 与 IPC 直查一致。非法输入必须进 `failed` 而非静默 `applied`。

### V3 Web 前端人机闭环

目的为确认运维人员只用浏览器能走完日常操作。方法为浏览器访问节点地址，登录默认账号并按提示改密，逐区操作并观察配置状态行：角色区（含待重启标记与撤销）、输入口区（含编码与目标同时填写才启动发送、只存采集口则不启动发送、发送状态标签）、输出设置区（含能力上限过滤、自定义范围校验、强推与热插拔）、网络与注册区（含静态格式校验、IP 冲突回滚、DHCP 确认、注册模式与立即注册）、SIP 区、本机信息区、维护操作区。每个保存后必须出现 `pending/applying/applied/failed` 之一，失败必须带可读原因。显示器轮次仍按 `manual_check_visual` 逐项问答，结论记入见证日志；非交互终端只记未见证，不伪造通过。

### V4 媒体独立模式（输出盒单路）

目的为确认引擎数据面在受控包之外仍能出画，隔离包问题与链路问题。方法为先跑裸链路基线（单板回环与双路合成中至少一项全绿），再用同一网络与显示参数起 `dvs_media --config <static>` 独立模式，布局含至少一路流。判定沿用裸链路口径：解码与收包之比、零丢零坏零反压、发送跟上目标帧率、渲染数小于解码数属按时钟取最新帧的主动跳帧只看趋势。音频存在时核对 TX 与 RX 帧数、零丢、重采样行、ALSA 实际消费。本层不过先查参数对齐，再查裸链路是否同样不过。

### V5 输入盒发送

目的为确认 TX 编排与角色互斥。方法为切 `role=input` 后经 `input` scope 下发采集口、编码与目标三元组，观察 `GET /api/v1/node/media/tx` 从 `idle` 到 `configured` 再到 `running`。只存采集口不填目标时必须停在已保存而不谎称发送中；参数超限（分辨率、帧率、码率、地址端口、GOP）必须在代理侧或引擎侧明确拒绝；非输入角色下启动必须报角色错。数据面一旦 `running`，对端必须能收到可组包的 ST22 与可选 ST30。缺 RGA/MPP 的降级语义（报 `configured+pending` 不谎称 `running`）只在 x86 单测缝出现，板上必须为真发送。

### V6 受控热更新

目的为确认布局可热更且屏障诚实。方法为受控模式空闲启动后经 `configure/prepare/commit` 下发首布局，再下发增删改布局与音频归属切换，观察 `query_state` 的 revision 单调、`lanes` 与 `audio/display` 快照为已应用值而非 pending 回显。并发 prepare 必须拒第二个；stale revision 必须拒；commit 前无 prepare 必须拒；已提交重放必须直接回已提交而不重建引擎。跨节点屏障由平台侧验证，本设计在单节点只验 `commit.report` 的帧号与时间戳字段存在。

每路 stream 必备 `main`（恰好一路为 true）、`sip/srcIp/udpPort/width/height/fps` 与有效渲染矩形（`x/y/w/h`，零矩形在 commit 期报 `invalid rect`）；缺任一项 prepare 或 commit 期诚实失败，状态不伪造。清场注意 `dvs_media` 忽略 SIGTERM，必须按 pid `kill -9`，再以 `board_env.sh status` 确认 DRM/UDP/声卡释放。

### V7 长稳

目的为确认不退化。方法为输出盒双路合成或输入盒发送任选其一跑半小时起步，采样 CPU、编解码负载、内存与句柄、欠载与跳帧增量。判定只有两条：产出比不随时间衰减；资源无单调泄漏。长稳不做主观画质评分。

## 7 dvs_agent Web 后端功能验证矩阵

下表为节点后台每一区对应的后端动词与板上断言。x86 无引擎基线已跑通（见第 9 章证据），板上有引擎时把 `engineLink=offline` 换成 `online` 且媒体查询面有真值，其余断言不变。

| 前端区 | 后端动词 | 板上动作与断言 |
|---|---|---|
| 登录 | `POST login/password/logout` | 正确密码进、错密码拒、首次标记改密、限流五次后锁；登出清 cookie |
| 节点角色 | `GET role`，`POST config/role` | 保存后 `pendingRole` 置位且待重启标记显现；撤销回原角色；重启后提升 |
| 输入口 | `POST config/input`，`GET media/tx/sources` | 编码加目标同时填才启动发送；非法编码、超限几何帧率码率、非法目标地址端口必须失败；`tx-state` 标签与查询面一致 |
| 输出设置 | `POST config/output`，`GET capability` | 超能力直接失败；后台线程按引擎结果翻转且 revision 守卫防旧覆盖；热插拔后能力变化重估 |
| 网络与注册 | `POST config/network/registration`，`POST register` | 静态格式错直接失败；冲突回滚；默认只落意图不切系统网（切网需隔离确认）；注册状态机可读 |
| SIP | `POST config/sip` | 关闭与启用两态；启用后连通性探测状态上报，不影响媒体 |
| 本机信息 | `GET info/capacity` | 型号、板型、SN、固件、角色、IP、MAC、注册态、当前播放、CPU 内存温度齐全 |
| 维护操作 | `POST reboot/factory-reset/upgrade/upload`，`GET audit-events` | 验证中不执行重启与恢复出厂；升级只验上传落盘、校验与 staging 语义；每次动作有审计 |
| 播放执行 | `GET media/status/lanes/sources` | 仅输出盒适用；当前播放投影与 lane 就绪数一致 |
| 版本与链路 | `GET version` | 包版本、IPC 代际、`match`、门控原因、`engineLink.failStreak/lastOkAgeMs` 齐全 |
| 能力 | `GET capability` | 输出入端口、解码上限、图层数、TX 上限、PTP、MTL 串齐全；过期标记诚实 |

六 scope 状态机统一断言为保存后先 `pending_apply`，引擎接受后 `applied`，拒绝后 `failed` 并带 `errorCode/errorReason`；`desiredRevision` 递增，`appliedRevision` 只在成功后跟进；旧 revision 回放不覆盖新值。输出 scope 经后台线程生效，验证时需轮询而非一次查询即判。

## 8 需要的调试脚本

### 8.1 复用（不新增，只调）

`board_env.sh status/run/release/restore` 作所有轮次门禁与一条龙入口；`drm_smoke`、`alsa_tone`、`hdmi_passthrough`、`v4l2` 矩阵作显示、音频、采集三路冒烟；`run_tx_rx_e2e.sh` 作发送数据面基线；`run_multinode_composite.sh` 作合成器基线；`check_*` 与 `verify_vrx_margin.py` 作离线日志检查。以上脚本与同步路径见 `manual-scripts-inventory.md`，本设计不复制其逻辑。

### 8.2 新增三个（已提供，见 `debug/tests/`）

| 脚本 | 形态与位置 | 输入输出与判定 |
|---|---|---|
| `dvs_node_smoke.sh` | shell，经 `debug/tests/` 同步后拷可执行副本进 `bin/`，只依赖 `curl` + `python3` | 输入为代理端口与 IPC 路径；依次做双 `--version` 同包、`hello/capability/query_state` 只读、`version/info/capability/media/status` 与静态前端 200 检查；任一步不符即非零退出并打印归属段；引擎离线包络（`MEDIA_ENGINE_*` 无 `requestId`）判诚实通过 |
| `dvs_ipc_probe.py` | 标准库 python，直跑不进 meson，位置同上 | 输入为 socket 路径与动词；发一行 JSON 读一行回包并做包络字段断言；用于代理未起时的引擎直查；无 socket 时明确失败退出 1 |
| `dvs_agent_api_smoke.sh` | shell，`curl` 套件，位置同上 | 输入为代理基地址；覆盖登录、六 scope 查询、非法输入反例（超大几何、非法 IP、非法角色）按 `ok:true + data.state=failed` 判定、媒体查询面在线/离线双分支、审计与本地投影；破坏性运维动词不执行；全绿才算通过 |

三脚本共同约束为准：仓库为源、板上不改 `src/`、`bin/` 中为可执行副本、`md5sum` 双端对齐；输出与当轮日志同目录存放；阈值收敛只改阈值文件，不在脚本硬编码。脚本只做“敲什么、看到什么算过”，判定口径以本文与既有 design 为准。

### 8.3 为什么必须新增

既有脚本只覆盖裸管线的采集编码发送接收显示，不懂包版本契约、不懂 IPC 状态机、不懂六 scope 的 revision 守卫与后台生效、不懂版本门控与 `engineLink`。没有这三脚本，版本混搭、IPC 代际错、旧 revision 覆盖、输出后台失败都会被误记为链路失败。

## 9 已完成的 x86 验证（Web 后端，详见 `dvs-node-validation-report.md`）

本机 `meson test` 全绿：`dvs_agent_basic` 与 `dvs_platform_basic` 各一项通过。`dvs_media` 各单测为 aarch64 产物，x86 侧不可跑，留板上跑。

代理 HTTP 冒烟（本机 18082 端口，无引擎 socket）结论如下，板上复用同一套断言，只把引擎相关项从离线诚实值换成在线真值：`dvs_node_smoke.sh OVERALL: PASS`，`dvs_agent_api_smoke.sh OVERALL: PASS`（26 项）；登录正确进错拒；`role` 初值为输出；`capacity` 含 CPU 内存负载与运行秒；无引擎时 `media/status` 与 `media/tx` 诚实报未配置（无 `requestId` 亦判通过），`lanes` 为空数组，`sources` 带不可达标记，`logs` 带链路与过期标记；静态前端根路径跳转且 `node-admin.html` 为 200；`audit-events` 与本地 `nodes` 投影可读；非法角色/输出几何/静态 IP 均进 `data.state=failed` 并带 `errorCode`。以上行为与“无引擎不谎报”要求一致，可作为板上对照基线。

## 10 失败定位顺序

先看版本与门禁：双 `--version`、`hello`、`version` 三处是否同包配对；再看 `board_env.sh status` 是否全绿；再看单路裸链路是否同样不过；再看参数对齐（宽高帧率、端口、收发地址、pacing、码流上限、播放延迟）；再看 IPC 与代理的 `failed` 原因行；最后才怀疑网络或争用。超时不退出先查 feeder 停机路径；中途无日志先查 stdout 缓冲（脚本已用行缓冲处理）。

## 11 取证与报告

每轮一页：拓扑（单板回环或双板）、源类型、参数摘要、版本与门禁快照、IPC 与代理查询面摘要、五级帧数、时延分位、音频状态、硬件负载、结论与定位。取证物为当轮 `OUT_DIR` 日志加 `perf/` 输出，归档到 `docs/archive/perf` 按日期命名。失败必须指明打包、IPC、代理、引擎、环境五段中的一段，并给出下一动作是重推重跑还是查环境。

## 附录 最小人工命令索引

引擎直查经 socket 发带 `type/requestId` 的行，读回带 `ok/status/code/msg/data/requestId` 的行；代理查询经 `GET`，保存经 `POST /api/v1/node/config/<scope>` 带 `desiredValue/operationId`，查询经 `GET /api/v1/node/config/<scope>` 看 `state/desiredRevision/appliedRevision/errorReason`。输出 scope 生效为后台线程，保存后轮询至多二十次。维护三动词验证中不执行。静态前端以 `pages/node-admin/node-admin.html` 为入口。
