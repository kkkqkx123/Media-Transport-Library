# MTL 项目同步指南（本地 <-> devs 编译服务器）

## 同步脚本
`scripts/sync_to_devs.sh`：按 git 变更（working tree + staged + untracked）逐文件 scp 到 `devs:/data/kkkqkx/mtl`。

```bash
./scripts/sync_to_devs.sh            # 同步变更文件
./scripts/sync_to_devs.sh --dry-run  # 仅打印
./scripts/sync_to_devs.sh --all      # 同步整个 tracked 树
./scripts/sync_to_devs.sh --init-git # 远端初始化 git + origin=gitlab
```

环境变量：`REMOTE`、`REMOTE_DIR`、`GITLAB_URL`、`BRANCH`。

## 远端约定
- `/data/kkkqkx/mtl` 为编译目录，git 仅保留初始提交，origin 指向 gitlab（`http://10.10.20.217/kkkqkx123/mtl-rk-ids.git`）。
- 系统库/依赖手动 scp，不在脚本范围。

## 提交流程
1. 本地改动 -> `./scripts/sync_to_devs.sh`
2. 远端：`cd /data/kkkqkx/mtl && git add -A && git commit -m '...' && git push origin <branch>`
3. 本地：`git pull gitlab <branch>` 收敛

## 远端 -> 本地
远程修改通过 `git diff` 生成 patch 或直接 scp 文件回本地；简单编辑可远程 python3/shell，但必须明确指定文件路径，禁止无范围批量编辑。
