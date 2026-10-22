## 1. 命令执行

```bash
# 单条命令
ssh board "uname -m"

# 多条命令（分号连接）
ssh board "hostname; uname -m; whoami"
```

---

## 2. 文件传输

```bash
# scp 单文件
scp local_file board:/opt/zlmediakit/

# scp 目录
scp -r local_dir board:/opt/zlmediakit/

# rsync（增量同步，推荐大目录）
rsync -avz --exclude='*.o' local_dir/ board:/opt/zlmediakit/
```

