# Q: 执行 `cd /dir1/dir2` 会触发哪些 FUSE 回调？

## A: 触发流程分析

Shell 执行 `cd /dir1/dir2` 时，底层大致经历两个阶段：**路径验证**和**切换目录**。对应的 FUSE 回调调用顺序如下：

### 1. 路径验证阶段（stat 系统调用）

Shell 在切换目录前，先调用 `stat("/dir1/dir2")` 验证目标路径存在且是目录。内核 VFS 层逐级解析路径，每解析一个分量就调用一次 `getattr`：

| 顺序 | FUSE 回调 | 路径参数 | 作用 |
|------|-----------|----------|------|
| ① | `fs_getattr` | `"/"` | 验证根目录存在且是目录 |
| ② | `fs_getattr` | `"/dir1"` | 验证 dir1 存在且是目录 |
| ③ | `fs_getattr` | `"/dir1/dir2"` | 验证 dir2 存在且是目录 |

> 本文件系统使用 FUSE 高级 API（路径级），路径解析由内核完成。内核对路径中每个分量依次执行 lookup，在 FUSE 中表现为逐级 `getattr` 调用。如果任何一级 `getattr` 返回错误（如 `-ENOENT`），后续调用不会发生。

### 2. 权限检查阶段（可选）

部分 Shell 实现会额外检查目标目录的执行权限：

| 顺序 | FUSE 回调 | 路径参数 | 作用 |
|------|-----------|----------|------|
| ④ | `fs_getattr` | `"/dir1/dir2"` | 检查 mode 中是否包含执行权限位（`x`） |

> 本次实现的 `fs_getattr` 会返回 `din->mode`，内核据此判断权限。当前未注册 `fs_access` 回调，因此内核回退到用 `getattr` 返回的 mode 自行判断。

### 3. 切换目录阶段（opendir + chdir）

Shell 确认路径合法后，执行实际的目录切换：

| 顺序 | FUSE 回调 | 路径参数 | 作用 |
|------|-----------|----------|------|
| ⑤ | `fs_opendir` | `"/dir1/dir2"` | 打开目标目录，返回文件描述符 |
| ⑥ | `fs_releasedir` | `"/dir1/dir2"` | 关闭目录描述符（时机取决于 Shell 实现） |

> `fs_opendir` 在本次实现中验证目标确实是目录并将 inode 号存入 `fi->fh`。`fs_releasedir` 为空操作，因为写穿模式已在操作时即时写入磁盘。

### 完整调用序列

```
cd /dir1/dir2
│
├── stat("/dir1/dir2")          ← Shell 路径验证
│   ├── fs_getattr("/")          ① 确认根目录
│   ├── fs_getattr("/dir1")      ② 确认 dir1 存在且是目录
│   └── fs_getattr("/dir1/dir2") ③ 确认 dir2 存在且是目录
│
├── (access check, optional)     ← 权限检查（可能复用 ③ 的 getattr 结果）
│
└── chdir("/dir1/dir2")          ← 切换目录
    ├── fs_opendir("/dir1/dir2") ⑤ 打开目标目录
    └── fs_releasedir(...)        ⑥ 关闭目录
```

### 注意事项

1. **未触发的回调**：`fs_readdir` 不会被调用。`cd` 只需要验证路径存在且是目录，不需要列出目录内容。`ls /dir1/dir2` 才会触发 `fs_readdir`。

2. **内核缓存**：如果近期已访问过 `/dir1`，内核可能缓存了其 dentry 信息，从而跳过对 `"/dir1"` 的 `getattr` 调用，直接命中缓存。首次访问时才会看到完整的逐级调用。

3. **低级 API 差异**：若使用 FUSE 低级 API（inode 级），路径解析会显式触发 `lookup` 回调而非 `getattr`，本文系统的阶段2 使用的是高级 API，无需关心此区别。
