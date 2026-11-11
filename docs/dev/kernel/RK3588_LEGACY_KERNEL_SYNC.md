# 旧 rk3588 内核仓库同步说明（已归档）

> 本文件是从 `docs/dev/KERNEL_REPO_SYNC_GUIDE.md` 迁出的旧说明，
> 描述 `rk3588_5.1_XDP` / `rk3588bsp` 两个仓库的 patch 流程。
> EN588 新 SDK（`EN588-linux-6.1`）的流程见 `docs/dev/KERNEL_REPO_SYNC_GUIDE.md`。
> 旧的两个仓库不再跟进。

## 远程 (devs) 约定

- `/data/kkkqkx/rk3588_5.1_XDP`、`/data/kkkqkx/rk3588bsp` 仅保留初始提交（base commit），不随意创建新提交。
- 远程 git 仓库仅作为变更监控：通过 `git diff` / `git format-patch` 生成 patch，再 scp 到本地。
- patch 存放目录：`/data/kkkqkx/rk3588-patches/`（远程），本地对应 `patches/rk3588*`。

## 本地 -> 远程（应用 patch）

1. 把本地 patch 复制到远程 patch 目录：
   ```bash
   scp patches/rk3588/*.patch devs:/data/kkkqkx/rk3588-patches/
   ```
2. 远程把仓库 reset 到 base commit：
   ```bash
   ssh devs "cd /data/kkkqkx/rk3588_5.1_XDP && git reset --hard <base-commit>"
   ```
3. 依次应用 patch：
   ```bash
   ssh devs "cd /data/kkkqkx/rk3588_5.1_XDP && git am /data/kkkqkx/rk3588-patches/*.patch"
   ```
   一套 patch 可一起应用；顺序以文件名编号为准。

## 远程 -> 本地（生成 patch）

在远程生成自 base 以来的变更 patch，scp 到本地：
```bash
ssh devs "cd /data/kkkqkx/rk3588_5.1_XDP && git format-patch <base-commit> -o /data/kkkqkx/rk3588-patches/"
scp 'devs:/data/kkkqkx/rk3588-patches/*.patch' patches/rk3588/
```

## 修改远程代码的流程

- **一般修改**：把远程文件 scp 到本地 -> 本地编辑 -> scp 回远程。禁止在远程直接批量编辑。
- **简单编辑**：可直接在远程用 `python3` 或 shell 命令编辑，但 **必须明确指定文件路径**，禁止无范围的批量编辑（如 `sed -i` 不带文件、`find ... -exec` 全局替换）。
- 编辑完成后，远程 `git diff` 生成 patch，scp 到本地，更新 `patches/` 与 README。
