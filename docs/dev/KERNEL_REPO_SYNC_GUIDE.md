# EN588 内核仓库（EN588-linux-6.1）同步指南

SDK 基本信息与旧补丁适用性见 `docs/dev/kernel/EN588_SDK_BASE.md`、
`docs/dev/kernel/EN588_PATCH_APPLICABILITY.md`。
旧 `rk3588_5.1_XDP` / `rk3588bsp` 的同步说明已归档到
`docs/dev/kernel/RK3588_LEGACY_KERNEL_SYNC.md`，不再跟进。

代码真相源：本地 `patches/en588/`（新 SDK 的补丁放这里，不复用 `patches/rk3588/`）。

## 远程 (devs) 约定

- 包源（只读）：`/data/kkkqkx/EN588-oepnsource-SDK/`（目录名拼写原文如此），内含两个 tgz 及其 `.md5`。
- 工作区：`/data/kkkqkx/EN588-linux-6.1`，上游只有一个提交（`de7b21fb0`），不随意创建新提交。
- ubuntu GNOME 预制镜像（`ubuntu22.04-gnome-opensource.tgz` 解出的单个 `.img`）不进 patch 流程。
- 注意 tgz 解包后只有 `.git/`、没有工作区文件，初次解包后必须
  `git reset --hard HEAD`（约 23 万文件，setsid 后台跑完再动）。

## 本地 -> 远程（应用 patch）

1. 把本地 patch 复制到远程 patch 目录：
   ```bash
   scp patches/en588/*.patch devs:/data/kkkqkx/en588-patches/
   ```
2. 远程把工作区 reset 到上游提交：
   ```bash
   ssh devs "cd /data/kkkqkx/EN588-linux-6.1 && git reset --hard de7b21fb0"
   ```
3. 依次应用 patch：
   ```bash
   ssh devs "cd /data/kkkqkx/EN588-linux-6.1 && git am /data/kkkqkx/en588-patches/*.patch"
   ```
   一套 patch 可一起应用；顺序以文件名编号为准。

## 远程 -> 本地（生成 patch）

在远程生成自上游提交以来的变更 patch，scp 到本地：
```bash
ssh devs "cd /data/kkkqkx/EN588-linux-6.1 && git format-patch de7b21fb0 -o /data/kkkqkx/en588-patches/"
scp 'devs:/data/kkkqkx/en588-patches/*.patch' patches/en588/
```

## 修改远程代码的流程

- **一般修改**：把远程文件 scp 到本地 -> 本地编辑 -> scp 回远程。禁止在远程直接批量编辑。
- **简单编辑**：可直接在远程用 `python3` 或 shell 命令编辑，但 **必须明确指定文件路径**，禁止无范围的批量编辑（如 `sed -i` 不带文件、`find ... -exec` 全局替换）。
- 编辑完成后，远程 `git diff` 生成 patch，scp 到本地，更新 `patches/en588/`。

## MTL 仓库本身

MTL 仓库同步仍使用 `scripts/sync_to_devs.sh`（按 git 变更文件逐个 scp），提交通过 gitlab 远程。
