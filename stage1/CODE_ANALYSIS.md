# MiniFS 代码分析文档

> 生成日期：2026-07-08  

> 项目路径：/root/myfs

---

## 一、项目概述

MiniFS 是一个基于 FUSE3 (Filesystem in Userspace) 的只读用户态文件系统。它使用自定义的磁盘镜像格式，通过 FUSE 框架将镜像中的数据以标准文件系统接口暴露给用户空间程序。

**核心特性：**
- 只读文件系统，禁止写入操作
- 64MB 固定大小磁盘镜像
- 4096 字节块大小
- 256 字节固定大小 Inode
- 仅支持一级目录（根目录下的文件）
- 12 个直接块指针（无间接寻址）


build 依赖（Ubuntu24.04）：
sudo apt update
sudo apt install gcc make pkg-config libfuse3-dev


---

## 二、文件结构

```
/root/myfs/
├── Makefile        # 构建脚本
├── fs_def.h        # 全局数据结构定义与常量
├── mkmyfs.c        # 镜像格式化工具
├── myfs.c           # FUSE3 文件系统实现
├── disk.img         # 运行时生成的磁盘镜像（64MB）
└── mnt/             # 挂载点目录
```

**编译产物：**
- `mkmyfs` — 镜像格式化可执行文件
- `myfs`  — FUSE 文件系统守护进程

---

## 三、磁盘布局

```
块号   偏移             内容
─────────────────────────────────────────────────────
0      0x000000        引导块（保留，未使用）
1      0x001000        超级块（struct superblock）
2      0x002000        Inode 位图（32768 bit）
3      0x003000        数据块位图（32768 bit）
4~     0x004000        Inode 表（32768 × 256B = 2048 块）
2052~  0x804000        数据区
...                    ...
16383  0xFFF000        镜像末尾
```

**布局计算：**

| 参数 | 值 | 计算方式 |
|------|----|----------|
| 镜像总大小 | 64 MB | `64 * 1024 * 1024` |
| 总块数 | 16384 | `64MB / 4096` |
| 预留块数 | 4 | 引导+超级块+2位图 |
| 最大 Inode 数 | 32768 | `4096 * 8`（一块位图） |
| Inode 表占用块数 | 2048 | `32768 * 256 / 4096` |
| 数据区起始块号 | 2052 | `4 + 2048` |
| 可用数据块数 | 14332 | `16384 - 2052` |
| 最大文件大小 | 48 KB | `12 × 4096`（仅直接块） |

---

## 四、数据结构详解

### 4.1 超级块 `struct superblock`

```c
struct superblock {
    uint32_t magic;              // 0x66757365 ("fuse")
    uint32_t block_size;         // 4096
    uint32_t total_inode;        // 32768
    uint32_t total_block;        // 16384
    uint32_t inode_table_start_blk;  // 4
    uint32_t data_start_blk;     // 2052
    uint8_t  pad[4072];          // 填充至 4096 字节
} __attribute__((packed));
```

- 整体占满一个 4096 字节块，有效字段仅前 24 字节
- `magic` 用于挂载时校验镜像合法性
- `packed` 属性确保无内存对齐间隙

### 4.2 Inode `struct d_inode`

```c
struct d_inode {
    uint16_t mode;               // 文件类型+权限 (如 0100644 / 040755)
    uint16_t nlink;              // 硬链接数
    uint32_t size;               // 文件大小（字节）
    uint32_t atime, mtime, ctime; // 访问/修改/状态变更时间
    uint32_t direct[12];         // 直接数据块指针
    uint8_t  pad[160];           // 填充至 256 字节
} __attribute__((packed));
```

- 固定 256 字节，`mode` 使用 Linux 标准格式（`040755` = 目录，`0100644` = 普通文件）
- 12 个直接块指针，最大寻址 48KB
- 无间接/二级间接块指针，不支持大文件

### 4.3 目录项 `struct dirent`

```c
struct dirent {
    uint32_t ino;                // Inode 号
    char     name[124];         // 文件名（最长 123 字节 + '\0'）
    uint16_t rec_len;           // 本条目长度（= sizeof(struct dirent) = 130）
} __attribute__((packed));
```

- 固定长度 130 字节，不支持变长目录项
- 每个数据块最多容纳 `4096 / 130 ≈ 31` 个目录项
- `rec_len` 字段用于遍历时的步进

---

## 五、源文件分析

### 5.1 `fs_def.h` — 公共头文件

**职责：** 定义所有全局常量、磁盘数据结构和工具函数声明。

| 宏/常量 | 值 | 说明 |
|---------|----|------|
| `BLOCK_SIZE` | 4096 | 磁盘块大小 |
| `IMG_TOTAL_SIZE` | 64MB | 镜像总大小 |
| `INODE_SIZE` | 256 | Inode 固定大小 |
| `PREFIX_BLOCKS` | 4 | 预留块数 |
| `BIT_PER_BLOCK` | 32768 | 单块位图可标记项数 |
| `ROOT_INO` | 1 | 根目录 Inode 号 |
| `MAGIC_NUM` | 0x66757365 | 魔数 "fuse" |

**声明的外部函数：** `bitmap_set`, `bitmap_get`, `read_inode`, `write_inode`

---

### 5.2 `mkmyfs.c` — 镜像格式化工具

**用法：** `./mkmyfs disk.img`

**执行流程：**

```
1. 创建/截断 64MB 镜像文件
2. 初始化并写入超级块到块1
3. 分配 Inode 1 给根目录
4. 分配数据块给根目录
5. 写入 "." 和 ".." 目录项
6. 分配 Inode 2 给 test.txt
7. 分配数据块给 test.txt
8. 写入文件内容
9. 在根目录追加 test.txt 目录项
10. 关闭镜像并输出结果
```

**关键函数分析：**

#### `bitmap_set(fd, blk_off, bit, val)`
- 在指定偏移处读取一个 4096 字节块
- 设置/清除指定位
- 写回磁盘
- **返回值：** 成功 0，失败 -1

#### `bitmap_get(fd, blk_off, bit)`
- 读取位图块，返回指定位的值 (0/1)
- **返回值：** 位值 (0/1) 或 -1 表示读取失败

#### `alloc_inode(fd)`
- 扫描 Inode 位图（块2），从 bit 1 开始查找空闲位
- **跳过 bit 0**：Inode 0 保留不使用
- 找到后置 1 并返回 Inode 号

#### `alloc_data_blk(fd, data_start)`
- 扫描数据位图（块3），从 `data_start` 对应的 bit 开始查找空闲位
- **关键修复点：** 原始版本从 bit 0 开始，会分配到预留块（超级块等），导致数据覆盖超级块。修复后从 `sb.data_start_blk`（2052）开始分配，确保数据块落在数据区
- 找到后置 1 并返回块号

#### `main()` 格式化逻辑
- 初始化超级块时计算：`data_start_blk = PREFIX_BLOCKS + (BIT_PER_BLOCK * INODE_SIZE / BLOCK_SIZE) = 4 + (32768 * 256 / 4096) = 4 + 2048 = 2052`
- 根目录创建时设置 `nlink = 2`（自身 + ".."）
- `test.txt` 内容为 `"Hello MiniFS!\nThis is read-only FUSE demo.\n"`（43 字节）

**格式化后的镜像状态：**

| 项 | 值 |
|----|----|
| Inode 位图 | bit 1, bit 2 已置 1 |
| 数据位图 | bit 2052, bit 2053 已置 1 |
| Inode 1 | 根目录, mode=040755, nlink=2, size=390 |
| Inode 2 | test.txt, mode=0100644, nlink=1, size=43 |
| 数据块 2052 | 根目录数据（3 个 dirent: ".", "..", "test.txt"） |
| 数据块 2053 | test.txt 文件内容 |

---

### 5.3 `myfs.c` — FUSE3 文件系统

**用法：** `./myfs disk.img mountpoint [fuse args]`

**架构：**

```
用户进程
  │  VFS 系统调用
  ▼
内核 FUSE 模块
  │  /dev/fuse
  ▼
myfs 用户态守护进程
  │  pread/pwrite
  ▼
disk.img 磁盘镜像
```

**全局状态：**
- `img_fd` — 镜像文件描述符（只读打开）
- `sb_cache` — 超级块缓存（挂载时读取一次）

**关键函数分析：**

#### `path_to_ino(path, out_ino)` — 路径解析
- 仅支持一级路径：`/` → ROOT_INO，`/filename` → 在根目录中查找
- 读取根目录 Inode，获取其数据块
- 线性扫描目录项，匹配文件名
- **限制：** 不支持多级目录（如 `/dir/file`），不支持符号链接

#### `fs_getattr(path, st, fi)` — 获取文件属性
- 路径解析 → 读取 Inode → 填充 `struct stat`
- 字段映射：mode/nlink/size/atime/mtime/ctime
- `st_blocks` 按 512 字节单位计算：`(size + 4095) / 4096 * 8`

#### `fs_readdir(path, buf, filler, ...)` — 读取目录
- 校验目标 Inode 是否为目录 (`S_ISDIR`)
- 读取目录数据块，遍历目录项
- 使用 `filler` 回调将每个条目名称填充到用户缓冲区
- **注意：** 不传递 `stat` 信息给 filler（第三个参数传 NULL）

#### `fs_open(path, fi)` — 打开文件
- 路径解析 + Inode 读取
- 拒绝写入请求：`O_WRONLY` 或 `O_RDWR` → 返回 `-EROFS`
- 将 Inode 号存入 `fi->fh` 供后续 `read` 使用

#### `fs_read(path, buf, size, offset, fi)` — 读取文件内容
- 从 `fi->fh` 获取 Inode 号（无需再次路径解析）
- 边界检查：offset >= size 返回 0；截断超出文件末尾的读取
- 块映射计算：`blk_id = offset / BLOCK_SIZE`，块内偏移 `blk_off = offset % BLOCK_SIZE`
- 通过 `direct[blk_id]` 获取物理块号，`pread` 读取
- **限制：** 若 `blk_id >= 12` 或 `direct[blk_id] == 0` 会产生越界读取或读到错误数据

**FUSE 操作注册：**

```c
static struct fuse_operations fs_ops = {
    .getattr = fs_getattr,   // stat
    .readdir = fs_readdir,   // ls
    .open    = fs_open,      // open (只读)
    .read    = fs_read,      // read
};
```

**未实现操作：** write, mkdir, unlink, rename, chmod, chown, truncate, symlink, readlink, statfs, flush, release, fsync, ioctl 等

---

## 六、构建系统分析 (`Makefile`)

```makefile
CC = gcc
CFLAGS = -Wall -O2 -std=c99
FUSE_CFLAGS = `pkg-config --cflags fuse3`
FUSE_LIBS = `pkg-config --libs fuse3`
```

| 目标 | 依赖 | 编译命令 |
|------|------|----------|
| `mkmyfs` | mkmyfs.c, fs_def.h | `gcc -Wall -O2 -std=c99 -o mkmyfs mkmyfs.c` |
| `myfs` | myfs.c, fs_def.h | `gcc -Wall -O2 -std=c99 $(FUSE_CFLAGS) -o myfs myfs.c $(FUSE_LIBS)` |

**依赖：** `pkg-config`, `libfuse3-dev`

---

## 七、编译问题修复记录

| # | 错误信息 | 根因 | 修复方式 |
|---|---------|------|---------|
| 1 | `pkg-config: not found` | 系统未安装 pkg-config | `apt install pkg-config libfuse3-dev` |
| 2 | `implicit declaration of function 'pread'` | `-std=c99` 隐藏 POSIX 函数声明 | 在 mkmyfs.c / myfs.c 头部添加 `#define _GNU_SOURCE` |
| 3 | `incomplete element type 'struct timespec'` | fuse3/fuse.h 需要 timespec 但 time.h 未提前包含 | 在 myfs.c 中 `#include <time.h>` 移到 `#include <fuse3/fuse.h>` 之前 |
| 4 | `'S_IFDIR' undeclared` | C99 标准不暴露 S_IFDIR 宏 | 改用标准宏 `S_ISDIR(din.mode)` |
| 5 | **超级块被覆盖导致 "Not MiniFS image!"** | `alloc_data_blk` 从 bit 0 开始分配，块 1（超级块）被分配给文件数据，超级块内容被覆盖 | `alloc_data_blk` 起始 bit 从 0 改为 `sb.data_start_blk`，确保仅分配数据区块 |

---

## 八、运行测试流程

```bash
# 1. 编译
make clean && make

# 2. 生成镜像
./mkmyfs disk.img
# 输出: Format success: disk.img
#        Root ino=1, test.txt ino=2

# 3. 挂载
mkdir -p mnt
./myfs disk.img mnt -f -s &
# -f: 前台运行（调试用）
# -s: 单线程模式

# 4. 测试
ls -la mnt/
# drwxr-xr-x  .  ..
# -rw-r--r--  test.txt  (43 bytes)

cat mnt/test.txt
# Hello MiniFS!
# This is read-only FUSE demo.

# 5. 卸载
fusermount -uz mnt
```

---

## 九、安全性与健壮性评估

### 9.1 当前已知限制

| 类别 | 限制 | 风险等级 |
|------|------|---------|
| 功能 | 只读文件系统，不支持任何写入操作 | 低（设计意图） |
| 功能 | 仅支持一级目录，无子目录 | 中 |
| 功能 | 最大文件 48KB（12 直接块） | 中 |
| 功能 | 最大 32768 个 Inode | 低（64MB 镜像足够） |
| 健壮性 | `fs_read` 未检查 `blk_id < 12` | 高（越界访问） |
| 健壮性 | `fs_read` 未检查 `direct[blk_id] != 0` | 高（读到垃圾数据） |
| 健壮性 | 路径解析不支持 `/` 结尾或重复 `/` | 低 |
| 健壮性 | 无 `statfs` 实现，`df` 无法显示信息 | 低 |
| 健壮性 | 工具函数在 myfs.c 中重复定义 | 低（代码冗余） |

### 9.2 潜在改进建议

1. **fs_read 块边界检查**：在 `blk_id` 越界或 `direct[blk_id] == 0` 时返回 0（EOF）而非越界读取
2. **多级目录支持**：扩展 `path_to_ino` 递归解析路径分量
3. **间接块指针**：添加一级/二级间接块以支持大文件
4. **statfs 实现**：返回文件系统统计信息，支持 `df` 命令
5. **代码复用**：将 bitmap/read_inode/write_inode 抽取到独立的 `fs_utils.c` 文件，消除 myfs.c 中的重复定义
6. **镜像校验**：挂载时检查 `total_block`、`data_start_blk` 等字段的合理性
7. **目录项变长支持**：使用变长 `rec_len` 提高目录空间利用率

---

## 十、架构图

```
┌─────────────────────────────────────────────┐
│              用户空间 (Userspace)              │
│                                              │
│   ┌──────────┐    ┌────────────────────┐    │
│   │  mkmyfs   │    │       myfs         │    │
│   │ (格式化)  │    │  (FUSE 守护进程)   │    │
│   └────┬─────┘    └────────┬───────────┘    │
│        │                   │ libfuse3        │
│        │ pwrite           │ /dev/fuse       │
│        ▼                   ▼                 │
│   ┌─────────────────────────────────────┐    │
│   │          disk.img (64MB)            │    │
│   │  [引导][超级块][Ino位图][数据位图]   │    │
│   │  [Inode 表 ···][数据区 ···]         │    │
│   └─────────────────────────────────────┘    │
│                     ▲                        │
└─────────────────────┼────────────────────────┘
                      │ pread (只读)
               ┌──────┴──────┐
               │  内核 VFS   │
               │  FUSE 模块  │
               └─────────────┘
                      ▲
                      │ 系统调用
               ┌──────┴──────┐
               │  用户进程    │
               │ ls, cat ... │
               └─────────────┘
```

---

## 十一、关键代码路径追踪

### 11.1 读取文件完整路径 (`cat mnt/test.txt`)

```
1. VFS → fs_open("/test.txt")
   ├── path_to_ino("/test.txt", &ino)
   │   ├── strcmp(path, "/") != 0  → 进入文件查找
   │   ├── name = "test.txt"
   │   ├── read_inode(img_fd, 1, &root)  → 读根 Inode
   │   ├── pread(img_fd, buf, 4096, root.direct[0]*4096)  → 读目录数据
   │   ├── 遍历目录项，strcmp("test.txt") 匹配 → ino=2
   │   └── return 0
   ├── read_inode(img_fd, 2, &din)  → 读 test.txt Inode
   ├── 检查 flags 无 O_WRONLY/O_RDWR → 只读允许
   ├── fi->fh = 2  → 缓存 Inode 号
   └── return 0

2. VFS → fs_read("/test.txt", buf, 4096, 0, fi)
   ├── ino = fi->fh = 2
   ├── read_inode(img_fd, 2, &din)  → 读 test.txt Inode
   ├── offset=0 < din.size=43 → 继续
   ├── size = min(4096, 43-0) = 43
   ├── blk_id = 0 / 4096 = 0
   ├── blk_off = 0 % 4096 = 0
   ├── data_start = din.direct[0] * 4096
   ├── pread(img_fd, buf, 43, data_start)  → 读 43 字节
   └── return 43

3. VFS → fs_read("/test.txt", buf, 4096, 43, fi)
   ├── offset=43 >= din.size=43
   └── return 0  → EOF
```

### 11.2 列出目录完整路径 (`ls mnt/`)

```
1. VFS → fs_getattr("/", st, NULL)
   ├── path_to_ino("/", &ino) → ino=1
   ├── read_inode(img_fd, 1, &din)
   └── 填充 st: mode=040755, nlink=2, size=390

2. VFS → fs_readdir("/", buf, filler, 0, NULL, 0)
   ├── path_to_ino("/", &dir_ino) → dir_ino=1
   ├── read_inode(img_fd, 1, &din)
   ├── S_ISDIR(040755) → true
   ├── pread(img_fd, block_buf, 4096, din.direct[0]*4096)
   ├── 遍历目录项:
   │   ├── ent[0]: ino=1, name=".",   rec_len=130 → filler(".")
   │   ├── ent[1]: ino=1, name="..",  rec_len=130 → filler("..")
   │   └── ent[2]: ino=2, name="test.txt", rec_len=130 → filler("test.txt")
   └── return 0
```

---

*文档结束*
