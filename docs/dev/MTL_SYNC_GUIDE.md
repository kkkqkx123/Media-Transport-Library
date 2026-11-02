# MTL 项目同步指南（本地 -> devs，单向）

## 同步脚本
`scripts/sync_to_devs.sh`：按 git 变更（working tree + staged + untracked）逐文件 scp 到 `devs:/data/kkkqkx/mtl`。仅本地 -> 远程单向，无反向操作。

```bash
./scripts/sync_to_devs.sh            # 同步变更文件
./scripts/sync_to_devs.sh --dry-run  # 仅打印
./scripts/sync_to_devs.sh --all      # 同步整个 tracked 树
./scripts/sync_to_devs.sh --init-git # 远端初始化 git + origin=gitlab
```

## 远端约定
- `/data/kkkqkx/mtl` 为编译目录；git 状态与本地仓库通过 gitlab 同步（远端 origin = 本地 gitlab 远程）。
- 系统库/依赖手动 scp，不在脚本范围。

## 提交流程
1. 本地改动 -> `./scripts/sync_to_devs.sh`
2. 远端仅拉取：`cd /data/kkkqkx/mtl && git pull origin <branch>`
3. 本地修改提交并 push：`git commit ... && git push gitlab <branch>`

编译在本地/devs 按需进行；同步仅单向，不从远程反向同步。
