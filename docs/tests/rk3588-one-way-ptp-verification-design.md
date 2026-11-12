# 单向 PTP + 交换机 TC/BC 同步验证设计

适用对象：`rk3588_common` 单向 slave（`one_way_ptp`）与虚拟 TAI（`mp4_ptp_time_ctx`
修正量），经 TX（`live_ingest --use-tai`）与 RX（`file_playout_rx` external 模式）接入。
本文只定验证分层、拓扑、取证口径与判定，不改生产代码。

输入：`docs/plan/rk3588/RK3588_ONE_WAY_PTP_SYNC_DESIGN.md`（实施设计），
`docs/archive/app/rk3588-ptp-verification-report.md`（板端阻塞基线：YT6801 无发送时间戳），
`docs/env/RK3588_PTP_STATUS_AND_COMPARISON.md`（方案对照）。
关系：双板互通与时延口径复用 `rk3588-tx-rx-e2e-verification-design.md` 与
`rk3588-multinode-composite-verification-design.md`，本文只新增时间同步的分层与判定。

## 1 目标与成功标准

回答三个问题：同步报文是否到达板端并被正确解析；slave 伺服是否收敛并保持；
收敛后的 TAI 是否在各节点一致到业务可用。

成功标准按组网分两档：有透明时钟时任意两节点 TAI 差稳定在亚毫秒内；
无透明时钟验证组网时差值稳定在数毫秒内且不发散。两档都要求 master 中断后
播出不中断、恢复后自动重锁。任一分层失败都能定位到网络投递、伺服收敛、
应用接入三段中的一段。

## 2 被测链路现状

时间源是 x86 上标准 ptp4l 纯 master（masterOnly，双步，组播或单播），
板端只跑自研单向 slave。master 与 slave 之间可能经过透明时钟或边界时钟交换机，
也可能直连。slave 经组播组或单播接收 Sync/Follow_Up/Announce，
用软件接收戳打 t2，累加 correctionField，输出平滑偏移给虚拟 TAI。
TX 与 RX 经各自 `ptp_get_time_fn` 读同一虚拟 TAI，时间轴天然同域。

## 3 验证分层

### L0 交叉编译（devs，已通过）

范围：`live_ingest`、`file_playout_rx`、`dvs_media` 三产物在 devs
交叉编译通过，产物为 aarch64 ELF，新文件零告警。
结论记录在本文附录，回归时重跑 `script/build_rk3588_cross.sh --stages apps`
加 `dvs_media` 配置检查即可，不需要重复全量验证。

### L1 网络投递（单板探针）

用独立探针（`debug/tests/ptp_sync_probe.c`，板上编译直跑，不依赖应用与 MTL）
确认三件事：PTP 组播或单播报文到达板端；Sync 与 Follow_Up 能按序号配对；
correctionField 在经过透明时钟时逐跳递增、直连时接近零。
本层失败只查网络：组播路由与 IGMP Snooping、VLAN 与端口配置、
master 地址与域是否一致。伺服未参与，本层不判定精度。

### L2 单板伺服收敛

起 TX 或 RX（external/`--use-tai`），观察 slave 日志：
从 free 到 locked 的收敛时间（正式组网要求分钟级内），
锁定后 `offset` 残差带（有 TC 时百微秒内，无 TC 时毫秒内），
`sync` 计数随同步率单调增长且 `rej` 不持续攀升。
本层失败查 slave 参数：域、nominal_delay 常量、master 选择（多 master 时粘滞），
以及板端 CPU 争用导致软件接收戳抖动过大。

### L3 双板同域差值

两板同起 slave 并锁定后，对比同一时刻虚拟 TAI 差值。
取证用双方 `PTP[]` 日志行的时间戳并排比对，或用 Python 校验脚本
（`app/rk3588_playout_rx/tests/check_ptp_sync.py`）批量判定。
判定线见第 5 节。本层失败且 L2 各自正常时，查交换机：
透明时钟是否对媒体 VLAN 生效、边界时钟下游是否追错上游、
是否存在不对称路径。

### L4 业务联动

在 L3 通过的组网上跑文件源 TX/RX 互通，检查 RTP 时间戳 TAI 域对齐
与端到端行为：60Hz 帧边界余量、播出缓冲与音画同步吸收残差。
复用既有 e2e 判定（`check_playout_log.py` 等），本层不新增阈值，
只要求 L3 通过后业务结论与单板回环基线一致。

### L5 保持与恢复

拔掉 master（停 ptp4l 或断组播源）进入 holdover：播出不中断，
状态行标记 holdover，外推漂移；恢复后自动重锁并打印恢复事件。
保持超时后转自由运行并标记未锁定。本层失败查保持阈值与漂移学习是否生效。

## 4 验证拓扑

master 在 x86 上跑 ptp4l 纯 master，同步率每秒八次，
域与应用配置一致（ST 2059-2 取 127，现状过渡可取 0 但两端必须相同）。
组播为默认（标准事件组播地址，交换机透明时钟对组播单播一视同仁均修正）；
已有单播 master 配置时 slave 用 `--ptp-master` 指定单播地址，不加入组播。
板端用 `--ptp-ifname` 绑定媒体网卡，避免管理口分流。
直连、普通交换机、透明时钟交换机、边界时钟四种组网按第 3 节分层逐级跑，
结论按组网分别记录，不混用阈值。直连基线先行（做法见附录 B），
上交换机后多出的残差才能明确归因。

## 5 取证口径与判定

取证只有两处日志：slave 十秒一行状态（含 locked、offset、drift、sync/rej 计数），
与应用启动时一行 `PTP[]`（含修正量与锁定标记）。
校验脚本对一批日志判定四条：出现过 locked；尾段 offset 在组网对应带内；
尾段无 holdover（L5 专项除外）；rej 占比不持续增长。
阈值：有透明时钟 offset 带取 1ms，验证组网取 5ms；跨节点 TAI 差阈值同带宽。
任一失败即 FAIL 并指出所属分层，不做模糊通过。

## 6 编排脚本

`app/rk3588_playout_rx/tests/run_ptp_sync_verify.sh` 编排 L1 到 L3：
预检（二进制、组播路由、master 可达）、探针采样、短时起应用收日志、调校验脚本出结论。
环境变量覆盖 master 地址、域、网卡与阈值。L5 单独用参数触发，
避免每次回归都等待保持超时。

## 附录 A L0 交叉编译结论（2026-11-12，devs）

产物：`live_ingest` 与 `file_playout_rx` 为 aarch64 ELF，
`dvs_media` 链接通过；新文件（`one_way_ptp`、虚拟 TAI 扩展）零告警。
环境修复（devs 本地，不污染仓库）：pip 装 `meson==0.61.5` 到
`/home/itc/.local/bin`（系统 meson 0.45.1 不支持 `cc.links`），
补 `importlib_metadata` 回退包。
仓库侧修复（已同步）：cross 文件模板键名 `pkg-config` 改为 `pkgconfig`、
`cpp_args` 内联（meson 不支持变量引用），`script/build_rk3588_cross.sh` 加执行位。
已知遗留（与本次改动无关，不追）：mtl 阶段 `-Denable_tests` 选项名过期、
`assert_products` 期望 `libmtl.so` 在 multiarch 路径而实际装在 `usr/lib`，
apps 链接不受影响。

## 附录 B 无交换机两设备基线（先行步骤）

直连是正式验证的第一步：把交换机变量清零，correctionField 恒为零，
测到的就是 slave 伺服本身的本底。上交换机后多出的残差才能明确归因给 TC/BC。

### C.1 两种组合

首选 x86 + 单板：x86 跑标准 ptp4l 纯 master（masterOnly，双步，组播，
同步率每秒八次，域与板端一致），直连网线到板。ptp4l 只在 master 侧需要发送时间戳，
x86 网卡一般满足；板端 slave 只收不发。master 时钟本身用 chrony 或 NTP 稳住，
否则 master 漂移会污染本底（slave 会跟随，残差仍可读但绝对值失真）。

纯双板：其中一板跑 lab mini master（`debug/tests/ptp_sync_master.c`，
用户态双步发送，软件戳，验证专用，生产仍用 x86 ptp4l）。ptp4l 在板上 master
同样跑不起来（也要发送时间戳），不要尝试。

### C.2 误差预算（直连，传播时延可忽略）

恒定 bias（一次标定消除）：master 发栈延迟（软件约 5~20µs）加板端收栈延迟
（约 10~50µs），合计几十微秒，由 `nominal_delay` 吸收。

抖动（标定不掉，滤波后残差即要测的偏差范围）：板端软件接收戳是主项，
空载约几十微秒，CPU 忙时数百微秒，偶发毫秒毛刺由中值窗加 5ms 剔除吃掉；
晶振漂移在同步间隔内只有数微秒，漂移估计器跟踪后残差可忽略。
预期：空载锁定后残差带约正负 50~200µs；`nominal_delay` 置零初跑时锁定值整体
偏置几十微秒，正好就是 bias 本身。

### C.3 测量程序

探针先行：板上跑探针 60 秒，应有稳定 pair 且 correction 接近零——
这行数据本身就是“路径上无隐藏交换机”的证据。

收敛与标定：起应用（文件源即可），`nominal_delay` 先置零跑 10 分钟，
按 5ms 带判定收敛时间（应分钟级锁定）与锁定 offset 均值（即 bias）；
把均值填入 `nominal_delay` 重跑，残差回到零附近。

slave 上报的 offset 本身就是“板 TAI 减主 TAI”的直接测量（微秒分辨率），
不需要两端同时采样再对表；未知常数 bias 靠标定分离。
SSH 两边同时 `date` 只能对到毫秒级，仅做粗交叉印证。

保持恢复：停 master 60 秒再恢复，验证 holdover 外推不断播、自动重锁（L5
在两设备下完全可测）。

### C.4 能证明与不能证明

能：报文可达与解析、收敛性与收敛时间、残差带（本底偏差范围）、标定方法、保持恢复。
不能：TC 修正是否生效、BC 分组隔离、多节点一致性、交换机排队抖动——
必须上交换机后按 L3/L4 重测。直连数据是本底下限，不是部署上限，报告里分开写。

## 附录 C 与既有资产的关系

双板互通拓扑与时延口径沿用 `rk3588-tx-rx-e2e-verification-design.md`，
多节点结论格式沿用 `rk3588-multinode-composite-verification-design.md`，
板端脚本索引见 `docs/tests/manual-scripts-inventory.md`（已补探针条目）。
linuxptp 双向方案在更换时间戳网卡后仍可回切，slave 下线即可，
验证分层中 L1/L3/L5 可直接复用。
