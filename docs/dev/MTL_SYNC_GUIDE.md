# MTL 项目同步指南（本地 -> devs，单向）

## 同步脚本
`scripts/sync_to_devs.sh`：按 git 变更（working tree + staged + untracked）逐文件 scp 到 `devs:/data/kkkqkx/mtl`。仅本地 -> 远程单向，无反向操作。

```bash
./scripts/sync_to_devs.sh            # 同步变更文件
./scripts/sync_to_devs.sh --dry-run  # 仅打印
./scripts/sync_to_devs.sh --all      # 同步整个 tracked 树
./scripts/sync_to_devs.sh --init-git # 远端初始化 git + origin=gitlab
```

## 同步方式选择（硬性规定）

- 日常只用增量同步：`./scripts/sync_to_devs.sh`（默认模式，仅同步相对 HEAD 的变更 + 新增文件）。单次变更几十个文件以内直接跑。
- 绝对禁止 `./scripts/sync_to_devs.sh --all` 全量 scp：数千文件逐个 scp 建连开销不可接受，本地资源紧张时更不允许。
- 两端差异较大时（devs 落后本地多个提交，或增量同步后仍对不上），一律在 devs 端用 git 追平，不用 scp 补：

```bash
ssh devs "cd /data/kkkqkx/mtl && git fetch origin && git reset --hard origin/feat/rk3588-apps"
```

  注意 `reset --hard` 会丢弃 devs 端未提交的修改：先确认要保留的未提交文件已在本地有备份（本地即源），重置后再用增量同步把这几个小文件推过去。新增的未跟踪文件不受 reset 影响，无需处理。

## 远端约定
- `/data/kkkqkx/mtl` 为编译目录；git 状态与本地仓库通过 gitlab 同步（远端 origin = 本地 gitlab 远程）。
- 系统库/依赖手动 scp，不在脚本范围。

## 提交流程
1. 本地改动 -> `./scripts/sync_to_devs.sh`
2. 远端仅拉取：`cd /data/kkkqkx/mtl && git pull origin <branch>`
3. 本地修改提交并 push：`git commit ... && git push gitlab <branch>`

编译在本地/devs 按需进行；同步仅单向，不从远程反向同步。
