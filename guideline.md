<aside>
🎯

**目标**：通过「自己动手从零实现一个精简文件系统」的方式，吃透 Linux 文件系统的核心原理（磁盘布局、inode、目录、位图、日志/崩溃一致性、缓存），并在实现中理解常见的优化方案（局部性、预读、延迟写、日志、COW、Extent、wear-leveling）。

**建议路线**：先用 **FUSE 在用户态**实现一个可挂载、可读写的最小文件系统（快速拿到反馈、易调试），再进阶到**内核态 VFS 模块**（真正理解 Linux 文件系统栈）。

</aside>

## 一、整体策略：两条腿走路

### 🥇 阶段路线 A（推荐先做）：用户态 FUSE

- **优点**：崩溃只挂掉进程、可用 gdb/print 调试、纯 C/Rust/Go、无需重启
- **产出**：一个把数据存进「一个大镜像文件」的 on-disk 文件系统
- **能学到**：磁盘布局、superblock、inode、bitmap、目录项、路径解析、read/write 语义

### 🥈 阶段路线 B（进阶）：内核态 VFS 模块

- **优点**：真正接触 Linux VFS 抽象（`super_operations` / `inode_operations` / `file_operations` / `address_space_operations`）
- **产出**：可 `insmod` + `mount -t myfs` 的内核模块
- **能学到**：VFS 对象模型、dentry cache、page cache、`mkfs` 工具编写

---

## 二、分阶段实现计划（含验收标准）

### 阶段 0 · 打地基（约 1 周）

- [ ]  通读 OSTEP 的 **File System Implementation / FFS / Journaling / LFS** 四章，建立心智模型
- [ ]  搭建环境：Linux VM / WSL2，安装 `libfuse-dev`、`build-essential`、`gcc/clang`、`gdb`、`qemu`（内核阶段用）
- [ ]  想清楚**磁盘布局草图**：`[ 引导块 | 超级块 | inode 位图 | 数据块位图 | inode 表 | 数据区 ]`
- **验收**：能在纸上画出布局，并说清「给定 inode 号如何算出它在镜像文件的字节偏移」

### 阶段 1 · 只读的最小 FUSE 文件系统（约 1 周）

- [ ]  用一个普通大文件（如 64MB）当「磁盘镜像」，定义 `struct superblock` / `struct d_inode`（固定大小）
- [ ]  写一个 `mkfs` 小工具：初始化超级块、位图，创建根目录 inode
- [ ]  实现 FUSE 回调：`getattr`、`readdir`、`open`、`read`
- **验收**：`mount` 后能 `ls` 根目录、`cat` 出预置文件内容

### 阶段 2 · 可写 + 目录树（约 1–2 周）

- [ ]  实现块/inode 分配器（扫描位图找空闲位）
- [ ]  实现 `create`、`write`、`truncate`、`unlink`、`mkdir`、`rmdir`、`rename`
- [ ]  文件寻址：先做**直接块**，再加**一级/二级间接块**（对照 ext2 的 inode 块指针树）
- **验收**：能 `mkdir -p a/b/c`、写入大文件、删除后空间可复用；`umount` 再 `mount` 数据仍在（持久化正确）

### 阶段 3 · 崩溃一致性（约 1–2 周，核心难点）

- [ ]  先用 `fsck` 思路：写一个离线检查器，验证位图与 inode 引用是否一致
- [ ]  再实现**日志（journaling）**：把「元数据更新」先写日志块，提交后再回写到主位置（对照 xv6 的 log 层）
- [ ]  用「随机 kill -9 / 断电模拟」测试：中断后重放日志能恢复到一致状态
- **验收**：在写操作中途强杀进程，重新挂载后文件系统仍一致、不丢已提交的数据

### 阶段 4 · 性能与优化专题（持续）

- [ ]  **空间局部性**：把同目录文件/inode 与数据尽量放近（对照 FFS 的 block group / 柱面组思想）
- [ ]  **预读（readahead）**：顺序读时批量预取后续块
- [ ]  **延迟写 / 写合并（write-back）**：脏块缓存 + 周期回刷，减少小写放大
- [ ]  **Extent 而非块指针**：用「起始块 + 长度」描述连续区间（对照 ext4）
- [ ]  **COW / 日志结构**：了解 LFS / Btrfs / littlefs 的写时复制与 wear-leveling 思路
- [ ]  用 `fio` / 自写基准脚本量化优化前后的吞吐与延迟
- **验收**：能给出「某项优化前后的基准数据 + 原理解释」

### 阶段 5 · 进阶到内核态（可选，约 2–4 周）

- [ ]  从 `psankar/simplefs` 的**第一个 commit**开始逐步跟读并自己重写
- [ ]  注册 `file_system_type`，实现 `mount` / `fill_super`，接上 VFS 各 operations 表
- [ ]  用 page cache 的 `read_folio` / `write_begin` / `write_end` 接管读写
- **验收**：`insmod myfs.ko` → `mkfs.myfs img` → `mount -t myfs` 成功并可读写

---

## 三、核心原理清单（边实现边对照）

| 主题 | 要点 | 在哪个阶段落地 |
| --- | --- | --- |
| Superblock | 记录布局参数、魔数、块大小、各区起止 | 阶段 1 |
| Inode | 元数据 + 数据块指针（直接/间接/Extent） | 阶段 1–2 |
| Bitmap 分配 | inode 位图 / 数据块位图，空闲管理 | 阶段 2 |
| 目录 | 目录即「名字 → inode 号」的特殊文件 | 阶段 2 |
| 路径解析 | 从根 inode 逐级 lookup | 阶段 2 |
| 崩溃一致性 | fsck / journaling / COW 三条路线 | 阶段 3 |
| 缓存 | buffer/page cache、脏页回写、预读 | 阶段 4–5 |
| VFS 抽象 | super/inode/file/address_space operations | 阶段 5 |

---

## 四、参考文档与开源资源

### 📘 教材 / 系统性原理

- **OSTEP（免费）** — 持久化部分的 File System Implementation、FFS、FSCK & Journaling、LFS、SSD 章节，原理讲得最清楚：pages.cs.wisc.edu/~remzi/OSTEP
- **xv6 book（MIT 6.1810）** — 第 6 章文件系统，分 7 层（disk/buffer/log/inode/dir/pathname/fd），日志层是崩溃一致性的极佳范例：xv6 book PDF

### 🛠️ 用户态 FUSE（阶段 A）

- **libfuse 官方仓库**（含 example/）：github.com/libfuse/libfuse
- **Pfeiffer «Writing a FUSE Filesystem» 教程**（经典入门）：cs.nmsu.edu/~pfeiffer/fuse-tutorial
- **IBM «Develop your own filesystem with FUSE»**：developer.ibm.com/articles/l-fuse
- **sh4dy «Creating a custom filesystem using FUSE»（分篇实战）**：sh4dy.com/2024/06/24/fuse_01
- **Kernel 文档 · FUSE**：kernel.org/doc/html/next/filesystems/fuse.html

### ⚙️ 内核态 VFS 模块（阶段 B）

- **psankar/simplefs** — 从零、逐 commit 学习的内核态 on-disk 文件系统（配合 kukuruku 博客系列）：github.com/psankar/simplefs
- **sysprog21/simplefs** — 维护更活跃、注释完善的教学用原生文件系统：github.com/sysprog21/simplefs
- **Linux Kernel Labs · File system drivers (Part 1)**：linux-kernel-labs.github.io/.../filesystems_part1
- **LKMPG（内核模块编程指南）**：sysprog21.github.io/lkmpg
- **Kernel 文档 · VFS 总览**：docs.kernel.org/filesystems/vfs.html

### 🧱 磁盘格式参考（照着写 on-disk 布局）

- **ext2 规范（NonGNU）**：nongnu.org/ext2-doc/ext2.html
- **Kernel 文档 · ext2**：docs.kernel.org/filesystems/ext2.html
- **OSDev Wiki · Ext2**：wiki.osdev.org/Ext2
- **Oracle «Understanding Ext4 Disk Layout»**（Extent 布局）：blogs.oracle.com/.../understanding-ext4-disk-layout-part-1

### 🔬 优化 / 现代设计思路

- **littlefs**（COW + 掉电安全 + wear-leveling，DESIGN.md 值得精读）：github.com/littlefs-project/littlefs
- **littlefs-fuse**（把 littlefs 挂到主机上用 hex 编辑器观察磁盘变化，绝佳调试方式）：github.com/littlefs-project/littlefs-fuse

---

<aside>
💡

**给你的建议**：先用 FUSE 在 2–3 周内把「阶段 1–3」跑通，拿到「能挂载、能读写、能崩溃恢复」的完整闭环，再回头对照 ext2/ext4 做优化专题。这样每一步都有可运行、可验证的产物，学习反馈最快。你也可以把每个阶段的踩坑与基准数据继续记录到本页，逐步长成一棵文件系统知识树。

</aside>