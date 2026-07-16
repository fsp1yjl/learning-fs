# MiniFS Stage2 — 可读写 + 目录树

> 阶段 2 目标：在只读版本基础上增加写操作和多层目录支持

## 与 Stage1 的关键变化

| 方面 | Stage1 | Stage2 |
|------|--------|--------|
| Inode 块指针 | 仅 12 个直接块 | 12 直接 + 1 一级间接 + 1 二级间接 |
| 最大文件大小 | 48 KB | ~4 GB（受 size 字段限制） |
| 路径解析 | 单级（/ 和 /filename） | 多级递归（/a/b/c） |
| 文件操作 | 只读（getattr/readdir/open/read） | 读写完整（create/write/truncate/unlink/mkdir/rmdir/rename） |
| 代码组织 | 工具函数在 mkmyfs.c 和 myfs.c 中重复 | 抽取到 `fs_utils.c/h` 共享 |
| 超级块 | 无空闲计数 | 记录 `free_inode` / `free_data_blk` 计数 |
| 磁盘镜像打开方式 | O_RDONLY | O_RDWR |

## 文件结构

```
stage2/
├── fs_def.h        # 磁盘数据结构定义（含间接块指针）
├── fs_utils.h      # 工具函数声明
├── fs_utils.c      # 工具函数实现（bitmap/inode/alloc/dir/path）
├── mkmyfs.c        # 镜像格式化工具
├── myfs.c           # FUSE3 文件系统守护进程
├── Makefile         # 构建脚本
└── README.md        # 本文档
```

## 磁盘布局（与 Stage1 一致）

```
块号    内容
─────────────────────────────
0      引导块（保留）
1      超级块（struct superblock，新增 free_inode/free_data_blk 字段）
2      Inode 位图（32768 bit）
3      数据块位图（32768 bit）
4~2051  Inode 表（32768 × 256B = 2048 块）
2052~  数据区（14332 块 ≈ 56MB）
```

## Inode 块指针树

```
                   d_inode
              ┌──────────────┐
              │ direct[0..11] │──→ 数据块 × 12  (0 ~ 11)
              │ indirect      │──→ [ptr×1024] ──→ 数据块 × 1024 (12 ~ 1035)
              │ dindirect     │──→ [ptr×1024] ──→ [ptr×1024] ──→ 数据块 × 1M (1036 ~ )
              └──────────────┘

寻址能力：
  直接:          12 块 × 4KB = 48 KB
  + 一级间接:  1024 块 × 4KB = 4 MB
  + 二级间接:  1048576 块 × 4KB ≈ 4 GB
  总计:          ≈ 4 GB（uint32_t size 字段限制到 4GB-1）
```

## 编译与运行

```bash
# 安装依赖
sudo apt update && sudo apt install gcc make pkg-config libfuse3-dev

# 编译
make clean && make

# 格式化镜像
./mkmyfs disk.img

# 挂载
mkdir -p mnt
./myfs disk.img mnt -f -s &
# -f: 前台运行（调试用）
# -s: 单线程模式（避免并发问题）

# 测试
ls -la mnt/
echo "hello world" > mnt/test.txt
cat mnt/test.txt
mkdir -p mnt/a/b/c
cp /etc/hostname mnt/a/b/hostname.txt
rm mnt/test.txt
df -h mnt/

# 卸载
fusermount -uz mnt
```

## 日志 / 调试

默认 `make` 产出的 `./myfs` 不含任何日志输出。如需观察 FUSE 回调的实际调用轨迹（用于调试，或与 `QA.md` 的回调分析对照），可编译带日志的调试版本：

```bash
make myfs_debug            # 生成 ./myfs_debug（-DDEBUG 开启日志）
./myfs_debug disk.img mnt -f -s
```

挂载后，每个 FUSE 回调入口会向 stderr 打印一行轨迹，形如：

```
[myfs] myfs: starting with image=disk.img
[myfs] getattr(/a)
[myfs] readdir(/a)
[myfs] create(/a/t, mode=666)
[myfs] write(/a/t, off=0, size=12)
```

- 日志开关为**编译期宏 `-DDEBUG`**：未定义时 `LOGF` 展开为空（零开销、不评估参数），调试日志不影响默认构建的运行性能。
- 输出定向到 stderr（默认无缓冲），与 FUSE 守护进程的 stdout 分离，便于 `2>trace.log` 单独收集。
- 可与 `QA.md` 中对 `cd /dir1/dir2` 触发回调序列的分析对照验证。

## 验收测试

```bash
# 1. 多级目录
mkdir -p mnt/a/b/c
ls -R mnt/a/

# 2. 写入大文件（需要间接块）
dd if=/dev/urandom of=mnt/bigfile bs=4096 count=2000   # ~8MB
md5sum mnt/bigfile

# 3. 删除后空间可复用
rm mnt/bigfile
echo "small" > mnt/newfile     # 应能成功

# 4. 持久化：umount 再 mount 数据仍在
fusermount -uz mnt
./myfs disk.img mnt -f -s &
cat mnt/newfile
```

## 已知限制 / TODO

- [ ] **truncate 间接块释放**：`fs_truncate` 目前只释放直接块中多余的块，间接块中的部分需要后续实现
- [ ] **目录空间回收**：删除目录项只标记 ino=0，不缩减目录 size，导致目录文件只会增长不会缩小
- [ ] **并发安全**：无锁保护，需以 `-s` 单线程模式运行；后续可加互斥锁
- [ ] **uid/gid**：inode 中无 uid/gid 字段，chown 操作为空实现
- [ ] **硬链接**：未实现 link()，nlink 逻辑简化
- [ ] **symlink**：未实现符号链接
- [ ] **fsync**：未实现显式刷盘
- [ ] **变长目录项**：当前 dirent 固定 130 字节，空间浪费较大（可对照 ext2 的变长 rec_len 优化）
