/*
 *  MiniFS Stage2 — 可读写 + 目录树 的 FUSE3 用户态文件系统
 *
 *  基于阶段 1 的只读版本，新增：
 *    • 多级目录路径解析（/a/b/c 逐级 lookup）
 *    • 块 / inode 分配器（扫描位图）+ 空间回收
 *    • 一级 / 二级间接块寻址（对照 ext2 inode 块指针树）
 *    • create / write / truncate / unlink / mkdir / rmdir / rename
 *    • statfs / utimens / release
 *
 *  用法: ./myfs disk.img mountpoint [fuse args]
 */

#define _GNU_SOURCE
#define FUSE_USE_VERSION 31
#include <time.h>
#include <fuse3/fuse.h>
#include "fs_def.h"
#include "fs_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* ================================================================
 *  全局状态
 * ================================================================ */
static int img_fd;                  /* 磁盘镜像文件描述符 */
static struct superblock sb_cache;  /* 超级块缓存 */

/* ================================================================
 *  辅助：将 d_inode 的字段填充到 struct stat
 * ================================================================ */
static void fill_stat(struct stat *st, uint32_t ino, struct d_inode *din)
{
    memset(st, 0, sizeof(*st));
    st->st_ino     = ino;
    st->st_mode    = din->mode;
    st->st_nlink   = din->nlink;
    st->st_size    = din->size;
    st->st_atime   = din->atime;
    st->st_mtime   = din->mtime;
    st->st_ctime   = din->ctime;
    st->st_blksize = BLOCK_SIZE;
    st->st_blocks  = (din->size + BLOCK_SIZE - 1) / BLOCK_SIZE * (BLOCK_SIZE / 512);
}

/* ================================================================
 *  FUSE 回调
 * ================================================================ */

/* ---- getattr: 获取文件/目录属性 ---- */
static int fs_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino;
    int ret = path_resolve(img_fd, path, &ino);
    if (ret < 0) return ret;

    struct d_inode din;
    if (read_inode(img_fd, ino, &din) != INODE_SIZE) return -EIO;

    fill_stat(st, ino, &din);
    return 0;
}

/* ---- readdir: 列出目录内容 ---- */
static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi; (void)flags;
    uint32_t dir_ino;
    int ret = path_resolve(img_fd, path, &dir_ino);
    if (ret < 0) return ret;

    struct d_inode din;
    if (read_inode(img_fd, dir_ino, &din) != INODE_SIZE) return -EIO;
    if (!S_ISDIR(din.mode)) return -ENOTDIR;

    uint32_t nblocks = (din.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (nblocks == 0) nblocks = 1;

    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t phys = inode_get_block(img_fd, &din, lb);
        if (phys == 0) continue;

        char blk[BLOCK_SIZE];
        if (read_block(img_fd, phys, blk) < 0) return -EIO;

        char *p = blk;
        while (p < blk + BLOCK_SIZE) {
            struct dirent *ent = (struct dirent *)p;
            if (ent->rec_len == 0) break;
            if (ent->ino != 0)       /* 跳过已删除条目 */
                /*
                 * 简单 readdir 模式：filler 第3个参数 stbuf 传 NULL，
                 * 仅传递文件名，不传 ino 等属性。FUSE 框架会对每个
                 * 目录项自动调用 fs_getattr 获取 ino/mode 等信息。
                 *
                 * 更高效的替代方案是 readdir-plus：
                 *   1) 读取 ent->ino 对应的 inode，填充 struct stat
                 *   2) filler(buf, ent->name, &st, 0, FUSE_FILL_DIR_PLUS)
                 * 这样 FUSE 无需额外调用 getattr，一次 readdir 即将
                 * 目录项+属性全部带回内核，目录项较多时性能更优。
                 *
                 * 注意：filler 返回非零值表示 buf 已满，应提前返回，
                 * 当前简化实现未检查该返回值。
                 */
                filler(buf, ent->name, NULL, 0, 0);
            p += ent->rec_len;
        }
    }
    return 0;
}

/* ---- open: 打开文件（读写均允许） ---- */
static int fs_open(const char *path, struct fuse_file_info *fi)
{
    uint32_t ino;
    int ret = path_resolve(img_fd, path, &ino);
    if (ret < 0) return ret;

    struct d_inode din;
    if (read_inode(img_fd, ino, &din) != INODE_SIZE) return -EIO;
    if (!S_ISREG(din.mode)) return -EISDIR;

    fi->fh = ino;
    return 0;
}

/* ---- read: 读取文件内容（支持间接块） ---- */
static int fs_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    (void)path;
    uint32_t ino = fi->fh;
    struct d_inode din;
    if (read_inode(img_fd, ino, &din) != INODE_SIZE) return -EIO;

    if (offset >= din.size) return 0;
    size_t remain = din.size - offset;
    if (size > remain) size = remain;

    size_t done = 0;
    while (done < size) {
        uint32_t lb   = (offset + done) / BLOCK_SIZE;
        off_t   boff = (offset + done) % BLOCK_SIZE;
        size_t  chunk = BLOCK_SIZE - boff;
        if (chunk > size - done) chunk = size - done;

        uint32_t phys = inode_get_block(img_fd, &din, lb);
        if (phys == 0) {
            /* 空洞：返回零 */
            memset(buf + done, 0, chunk);
        } else {
            ssize_t r = pread(img_fd, buf + done, chunk,
                              (uint64_t)phys * BLOCK_SIZE + boff);
            if (r < 0) return -EIO;
            if ((size_t)r < chunk) break;   /* 短读 */
        }
        done += chunk;
    }
    return done;
}

/* ---- write: 写入文件内容（自动分配块） ---- */
static int fs_write(const char *path, const char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    uint32_t ino = fi->fh;
    struct d_inode din;
    if (read_inode(img_fd, ino, &din) != INODE_SIZE) return -EIO;

    size_t done = 0;
    while (done < size) {
        uint32_t lb   = (offset + done) / BLOCK_SIZE;
        off_t   boff = (offset + done) % BLOCK_SIZE;
        size_t  chunk = BLOCK_SIZE - boff;
        if (chunk > size - done) chunk = size - done;

        /* 确保逻辑块已映射到物理块 */
        uint32_t phys;
        int ret = inode_alloc_block(img_fd, &sb_cache, &din, lb, &phys);
        if (ret < 0) {
            /* 空间不足，先写回已完成的 */
            if (done > 0) break;
            return ret;
        }

        if (chunk == BLOCK_SIZE) {
            /* 整块写入 */
            if (write_block(img_fd, phys, buf + done) < 0) return -EIO;
        } else {
            /* 部分写入：先读原块，再修改，再写回 */
            char blk[BLOCK_SIZE];
            if (read_block(img_fd, phys, blk) < 0) return -EIO;
            memcpy(blk + boff, buf + done, chunk);
            if (write_block(img_fd, phys, blk) < 0) return -EIO;
        }
        done += chunk;
    }

    /* 更新 inode 大小和时间 */
    if (offset + done > din.size)
        din.size = offset + done;
    din.mtime = din.ctime = time(NULL);
    write_inode(img_fd, ino, &din);
    write_superblock(img_fd, &sb_cache);

    return done;
}

/* ---- create: 创建新文件 ---- */
static int fs_create(const char *path, mode_t mode,
                     struct fuse_file_info *fi)
{
    uint32_t parent_ino;
    char name[MAX_NAME_LEN + 1];
    int ret = path_parent_resolve(img_fd, path, &parent_ino, name);
    if (ret < 0) return ret;

    /* 确认父目录是目录 */
    struct d_inode parent;
    if (read_inode(img_fd, parent_ino, &parent) != INODE_SIZE) return -EIO;
    if (!S_ISDIR(parent.mode)) return -ENOTDIR;

    /* 分配 inode */
    uint32_t new_ino = alloc_inode(img_fd, &sb_cache);
    if (new_ino == 0) return -ENOSPC;

    /* 初始化 inode */
    struct d_inode new_in = {0};
    new_in.mode  = S_IFREG | mode;
    new_in.nlink = 1;
    new_in.atime = new_in.mtime = new_in.ctime = time(NULL);
    new_in.size  = 0;
    write_inode(img_fd, new_ino, &new_in);

    /* 在父目录添加目录项 */
    ret = dir_add_entry(img_fd, &sb_cache, parent_ino, name, new_ino);
    if (ret < 0) {
        /* 回滚：释放刚分配的 inode */
        free_inode(img_fd, &sb_cache, new_ino);
        write_superblock(img_fd, &sb_cache);
        return ret;
    }

    fi->fh = new_ino;
    write_superblock(img_fd, &sb_cache);
    return 0;
}

/* ---- mkdir: 创建新目录 ---- */
static int fs_mkdir(const char *path, mode_t mode)
{
    uint32_t parent_ino;
    char name[MAX_NAME_LEN + 1];
    int ret = path_parent_resolve(img_fd, path, &parent_ino, name);
    if (ret < 0) return ret;

    struct d_inode parent;
    if (read_inode(img_fd, parent_ino, &parent) != INODE_SIZE) return -EIO;
    if (!S_ISDIR(parent.mode)) return -ENOTDIR;

    /* 分配 inode */
    uint32_t new_ino = alloc_inode(img_fd, &sb_cache);
    if (new_ino == 0) return -ENOSPC;

    /* 分配数据块 */
    uint32_t data_blk = alloc_data_blk(img_fd, &sb_cache);
    if (data_blk == 0) {
        free_inode(img_fd, &sb_cache, new_ino);
        write_superblock(img_fd, &sb_cache);
        return -ENOSPC;
    }

    /* 初始化目录 inode */
    struct d_inode new_in = {0};
    new_in.mode  = S_IFDIR | mode;
    new_in.nlink = 2;             /* . 和 .. */
    new_in.atime = new_in.mtime = new_in.ctime = time(NULL);
    new_in.size  = sizeof(struct dirent) * 2;
    new_in.direct[0] = data_blk;
    write_inode(img_fd, new_ino, &new_in);

    /* 写入 "." 和 ".." 目录项 */
    char dir_buf[BLOCK_SIZE] = {0};
    struct dirent *dot    = (struct dirent *)dir_buf;
    dot->ino      = new_ino;
    strcpy(dot->name, ".");
    dot->rec_len  = sizeof(struct dirent);

    struct dirent *dotdot = (struct dirent *)(dir_buf + sizeof(struct dirent));
    dotdot->ino    = parent_ino;
    strcpy(dotdot->name, "..");
    dotdot->rec_len = sizeof(struct dirent);

    write_block(img_fd, data_blk, dir_buf);

    /* 在父目录添加新目录项 */
    ret = dir_add_entry(img_fd, &sb_cache, parent_ino, name, new_ino);
    if (ret < 0) {
        free_data_blk(img_fd, &sb_cache, data_blk);
        free_inode(img_fd, &sb_cache, new_ino);
        write_superblock(img_fd, &sb_cache);
        return ret;
    }

    /* 父目录 nlink +1（因为新目录的 .. 指向父目录）
     * 注意：dir_add_entry 已经写回了父 inode，这里需要重新读取最新版本 */
    struct d_inode parent_updated;
    if (read_inode(img_fd, parent_ino, &parent_updated) == INODE_SIZE) {
        parent_updated.nlink++;
        parent_updated.mtime = parent_updated.ctime = time(NULL);
        write_inode(img_fd, parent_ino, &parent_updated);
    }

    write_superblock(img_fd, &sb_cache);
    return 0;
}

/* ---- unlink: 删除文件 ---- */
static int fs_unlink(const char *path)
{
    uint32_t parent_ino;
    char name[MAX_NAME_LEN + 1];
    int ret = path_parent_resolve(img_fd, path, &parent_ino, name);
    if (ret < 0) return ret;

    /* 查找目标 inode */
    uint32_t ino;
    ret = dir_lookup(img_fd, parent_ino, name, &ino);
    if (ret < 0) return ret;

    struct d_inode din;
    if (read_inode(img_fd, ino, &din) != INODE_SIZE) return -EIO;
    if (S_ISDIR(din.mode)) return -EISDIR;   /* 目录用 rmdir */

    /* 从父目录删除目录项 */
    ret = dir_del_entry(img_fd, parent_ino, name);
    if (ret < 0) return ret;

    /* 减少 nlink；若为 0 则释放资源 */
    din.nlink--;
    if (din.nlink == 0) {
        inode_free_all_blocks(img_fd, &sb_cache, &din);
        free_inode(img_fd, &sb_cache, ino);
    }
    write_inode(img_fd, ino, &din);
    write_superblock(img_fd, &sb_cache);
    return 0;
}

/* ---- rmdir: 删除空目录 ---- */
static int fs_rmdir(const char *path)
{
    uint32_t parent_ino;
    char name[MAX_NAME_LEN + 1];
    int ret = path_parent_resolve(img_fd, path, &parent_ino, name);
    if (ret < 0) return ret;

    /* 查找目标 inode */
    uint32_t ino;
    ret = dir_lookup(img_fd, parent_ino, name, &ino);
    if (ret < 0) return ret;

    struct d_inode din;
    if (read_inode(img_fd, ino, &din) != INODE_SIZE) return -EIO;
    if (!S_ISDIR(din.mode)) return -ENOTDIR;

    /* 检查目录是否为空 */
    int empty = dir_is_empty(img_fd, ino);
    if (empty < 0)  return empty;   /* 出错 */
    if (empty == 0) return -ENOTEMPTY;

    /* 从父目录删除目录项 */
    ret = dir_del_entry(img_fd, parent_ino, name);
    if (ret < 0) return ret;

    /* 释放目录资源 */
    inode_free_all_blocks(img_fd, &sb_cache, &din);
    free_inode(img_fd, &sb_cache, ino);

    /* 父目录 nlink -1（因为被删目录的 .. 不再指向父目录） */
    struct d_inode parent;
    if (read_inode(img_fd, parent_ino, &parent) == INODE_SIZE) {
        parent.nlink--;
        parent.mtime = parent.ctime = time(NULL);
        write_inode(img_fd, parent_ino, &parent);
    }

    write_inode(img_fd, ino, &din);
    write_superblock(img_fd, &sb_cache);
    return 0;
}

/* ---- truncate: 改变文件大小 ---- */
static int fs_truncate(const char *path, off_t size,
                       struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino;
    int ret = path_resolve(img_fd, path, &ino);
    if (ret < 0) return ret;

    struct d_inode din;
    if (read_inode(img_fd, ino, &din) != INODE_SIZE) return -EIO;
    if (!S_ISREG(din.mode)) return -EISDIR;

    if (size == 0) {
        /* 截断到 0：释放所有数据块 */
        inode_free_all_blocks(img_fd, &sb_cache, &din);
        din.size = 0;
    } else if (size < din.size) {
        /* 截短：释放超出部分的块 */
        uint32_t old_nblocks = (din.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        uint32_t new_nblocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        /* 释放超出 new_nblocks 的块 — 简化实现：只处理直接块 */
        for (uint32_t lb = new_nblocks; lb < old_nblocks && lb < 12; lb++) {
            if (din.direct[lb] != 0) {
                free_data_blk(img_fd, &sb_cache, din.direct[lb]);
                din.direct[lb] = 0;
            }
        }
        /* TODO: 释放间接块中的多余块 */
        din.size = size;
    } else {
        /* 扩展：只需增大 size，实际块在 write 时按需分配 */
        din.size = size;
    }

    din.mtime = din.ctime = time(NULL);
    write_inode(img_fd, ino, &din);
    write_superblock(img_fd, &sb_cache);
    return 0;
}

/* ---- rename: 重命名 / 移动 ---- */
static int fs_rename(const char *from, const char *to,
                     unsigned int flags)
{
    (void)flags;

    /* 解析源和目标 */
    uint32_t src_parent_ino, dst_parent_ino;
    char src_name[MAX_NAME_LEN + 1], dst_name[MAX_NAME_LEN + 1];

    int ret = path_parent_resolve(img_fd, from, &src_parent_ino, src_name);
    if (ret < 0) return ret;
    ret = path_parent_resolve(img_fd, to, &dst_parent_ino, dst_name);
    if (ret < 0) return ret;

    /* 查找源 inode */
    uint32_t src_ino;
    ret = dir_lookup(img_fd, src_parent_ino, src_name, &src_ino);
    if (ret < 0) return ret;

    /* 检查目标是否已存在 */
    uint32_t dst_ino;
    int dst_exists = dir_lookup(img_fd, dst_parent_ino, dst_name, &dst_ino);
    if (dst_exists == 0) {
        /* 目标已存在，先删除 */
        struct d_inode dst_din;
        if (read_inode(img_fd, dst_ino, &dst_din) != INODE_SIZE) return -EIO;
        if (S_ISDIR(dst_din.mode)) {
            if (!dir_is_empty(img_fd, dst_ino))
                return -ENOTEMPTY;
            /* 删除目标目录 */
            dir_del_entry(img_fd, dst_parent_ino, dst_name);
            inode_free_all_blocks(img_fd, &sb_cache, &dst_din);
            free_inode(img_fd, &sb_cache, dst_ino);
            struct d_inode dst_parent;
            if (read_inode(img_fd, dst_parent_ino, &dst_parent) == INODE_SIZE) {
                dst_parent.nlink--;
                write_inode(img_fd, dst_parent_ino, &dst_parent);
            }
        } else {
            dir_del_entry(img_fd, dst_parent_ino, dst_name);
            dst_din.nlink--;
            if (dst_din.nlink == 0) {
                inode_free_all_blocks(img_fd, &sb_cache, &dst_din);
                free_inode(img_fd, &sb_cache, dst_ino);
            }
            write_inode(img_fd, dst_ino, &dst_din);
        }
    }

    /* 从源目录删除 */
    ret = dir_del_entry(img_fd, src_parent_ino, src_name);
    if (ret < 0) return ret;

    /* 在目标目录添加 */
    ret = dir_add_entry(img_fd, &sb_cache, dst_parent_ino, dst_name, src_ino);
    if (ret < 0) return ret;

    /* 如果移动的是目录且父目录不同，更新 .. 和 nlink */
    struct d_inode src_din;
    if (read_inode(img_fd, src_ino, &src_din) == INODE_SIZE && S_ISDIR(src_din.mode)) {
        if (src_parent_ino != dst_parent_ino) {
            /* 更新被移动目录的 .. 指向新父目录 */
            uint32_t phys = inode_get_block(img_fd, &src_din, 0);
            if (phys != 0) {
                char buf[BLOCK_SIZE];
                if (read_block(img_fd, phys, buf) == 0) {
                    struct dirent *dotdot = (struct dirent *)(buf + sizeof(struct dirent));
                    dotdot->ino = dst_parent_ino;
                    write_block(img_fd, phys, buf);
                }
            }
            /* 旧父 nlink-1，新父 nlink+1 */
            struct d_inode old_parent, new_parent;
            if (read_inode(img_fd, src_parent_ino, &old_parent) == INODE_SIZE) {
                old_parent.nlink--;
                old_parent.mtime = old_parent.ctime = time(NULL);
                write_inode(img_fd, src_parent_ino, &old_parent);
            }
            if (read_inode(img_fd, dst_parent_ino, &new_parent) == INODE_SIZE) {
                new_parent.nlink++;
                new_parent.mtime = new_parent.ctime = time(NULL);
                write_inode(img_fd, dst_parent_ino, &new_parent);
            }
        }
    }

    src_din.ctime = time(NULL);
    write_inode(img_fd, src_ino, &src_din);
    write_superblock(img_fd, &sb_cache);
    return 0;
}

/* ---- statfs: 文件系统统计信息 ---- */
static int fs_statfs(const char *path, struct statvfs *st)
{
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize   = BLOCK_SIZE;
    st->f_blocks  = sb_cache.total_block - sb_cache.data_start_blk;
    st->f_bfree   = sb_cache.free_data_blk;
    st->f_bavail  = sb_cache.free_data_blk;
    st->f_files   = sb_cache.total_inode;
    st->f_ffree   = sb_cache.free_inode;
    st->f_fsid    = MAGIC_NUM;
    st->f_namemax = MAX_NAME_LEN;
    return 0;
}

/* ---- utimens: 修改时间戳 ---- */
static int fs_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino;
    int ret = path_resolve(img_fd, path, &ino);
    if (ret < 0) return ret;

    struct d_inode din;
    if (read_inode(img_fd, ino, &din) != INODE_SIZE) return -EIO;

    din.atime = tv[0].tv_sec;
    din.mtime = tv[1].tv_sec;
    din.ctime = time(NULL);
    write_inode(img_fd, ino, &din);
    return 0;
}

/* ---- release: 关闭文件（无操作，写穿模式已即时写入） ---- */
static int fs_release(const char *path, struct fuse_file_info *fi)
{
    (void)path; (void)fi;
    return 0;
}

/* ---- opendir: 打开目录 ---- */
static int fs_opendir(const char *path, struct fuse_file_info *fi)
{
    uint32_t ino;
    int ret = path_resolve(img_fd, path, &ino);
    if (ret < 0) return ret;

    struct d_inode din;
    if (read_inode(img_fd, ino, &din) != INODE_SIZE) return -EIO;
    if (!S_ISDIR(din.mode)) return -ENOTDIR;

    fi->fh = ino;
    return 0;
}

/* ---- releasedir: 关闭目录 ---- */
static int fs_releasedir(const char *path, struct fuse_file_info *fi)
{
    (void)path; (void)fi;
    return 0;
}

/* ---- chmod: 修改权限 ---- */
static int fs_chmod(const char *path, mode_t mode,
                    struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino;
    int ret = path_resolve(img_fd, path, &ino);
    if (ret < 0) return ret;

    struct d_inode din;
    if (read_inode(img_fd, ino, &din) != INODE_SIZE) return -EIO;

    /* 保留文件类型，仅修改权限位 */
    din.mode = (din.mode & S_IFMT) | (mode & 07777);
    din.ctime = time(NULL);
    write_inode(img_fd, ino, &din);
    return 0;
}

/* ---- chown: 修改属主（学习阶段简化：不实际存储） ---- */
static int fs_chown(const char *path, uid_t uid, gid_t gid,
                    struct fuse_file_info *fi)
{
    (void)path; (void)uid; (void)gid; (void)fi;
    /* 当前 inode 结构中没有 uid/gid 字段，直接返回成功 */
    return 0;
}

/* ================================================================
 *  FUSE 操作表
 * ================================================================ */
static struct fuse_operations fs_ops = {
    .getattr    = fs_getattr,
    .readdir    = fs_readdir,
    .open       = fs_open,
    .read       = fs_read,
    .write      = fs_write,
    .create     = fs_create,
    .mkdir      = fs_mkdir,
    .unlink     = fs_unlink,
    .rmdir      = fs_rmdir,
    .truncate   = fs_truncate,
    .rename     = fs_rename,
    .statfs     = fs_statfs,
    .utimens    = fs_utimens,
    .release    = fs_release,
    .opendir    = fs_opendir,
    .releasedir = fs_releasedir,
    .chmod      = fs_chmod,
    .chown      = fs_chown,
};

/* ================================================================
 *  main: 解析参数、读取超级块、启动 FUSE
 * ================================================================ */
int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s disk.img mountpoint [fuse args]\n", argv[0]);
        return 1;
    }

    /* 以读写方式打开镜像 */
    img_fd = open(argv[1], O_RDWR);
    if (img_fd < 0) { perror("open img"); return 1; }

    /* 读取超级块 */
    if (read_superblock(img_fd, &sb_cache) < 0) {
        perror("read superblock");
        close(img_fd);
        return 1;
    }
    if (sb_cache.magic != MAGIC_NUM) {
        fprintf(stderr, "Not MiniFS image!\n");
        close(img_fd);
        return 1;
    }

    /* 将 FUSE 参数跳过前两个（disk.img 和 mountpoint） */
    struct fuse_args args = FUSE_ARGS_INIT(argc - 1, argv + 1);
    int ret = fuse_main_real(args.argc, args.argv, &fs_ops,
                             sizeof(fs_ops), NULL);
    fuse_opt_free_args(&args);
    close(img_fd);
    return ret;
}
