#define _GNU_SOURCE
#include "fs_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

/* ==================================================================
 *  位图操作
 * ================================================================== */

int bitmap_set(int fd, uint64_t byte_off, uint32_t bit, int val)
{
    uint8_t buf[BLOCK_SIZE];
    if (pread(fd, buf, BLOCK_SIZE, byte_off) != BLOCK_SIZE)
        return -1;
    uint32_t byte_idx = bit / 8;
    uint32_t bit_idx  = bit % 8;
    if (val) buf[byte_idx] |=  (1u << bit_idx);
    else     buf[byte_idx] &= ~(1u << bit_idx);
    if (pwrite(fd, buf, BLOCK_SIZE, byte_off) != BLOCK_SIZE)
        return -1;
    return 0;
}

int bitmap_get(int fd, uint64_t byte_off, uint32_t bit)
{
    uint8_t buf[BLOCK_SIZE];
    if (pread(fd, buf, BLOCK_SIZE, byte_off) != BLOCK_SIZE)
        return -1;
    uint32_t byte_idx = bit / 8;
    uint32_t bit_idx  = bit % 8;
    return (buf[byte_idx] >> bit_idx) & 1;
}

/* ==================================================================
 *  Inode 读写
 *  Inode 号从 1 开始；偏移 = PREFIX_BLOCKS * BLOCK_SIZE + (ino-1) * INODE_SIZE
 * ================================================================== */

ssize_t read_inode(int fd, uint32_t ino, struct d_inode *out)
{
    if (ino == 0) return -1;
    uint64_t off = (uint64_t)PREFIX_BLOCKS * BLOCK_SIZE
                 + (uint64_t)(ino - 1) * INODE_SIZE;
    return pread(fd, out, INODE_SIZE, off);
}

ssize_t write_inode(int fd, uint32_t ino, struct d_inode *in)
{
    if (ino == 0) return -1;
    uint64_t off = (uint64_t)PREFIX_BLOCKS * BLOCK_SIZE
                 + (uint64_t)(ino - 1) * INODE_SIZE;
    return pwrite(fd, in, INODE_SIZE, off);
}

/* ==================================================================
 *  超级块读写
 * ================================================================== */

int read_superblock(int fd, struct superblock *sb)
{
    return pread(fd, sb, sizeof(*sb), 1 * BLOCK_SIZE) == sizeof(*sb) ? 0 : -1;
}

int write_superblock(int fd, const struct superblock *sb)
{
    return pwrite(fd, sb, sizeof(*sb), 1 * BLOCK_SIZE) == sizeof(*sb) ? 0 : -1;
}

/* ==================================================================
 *  数据块读写
 * ================================================================== */

int read_block(int fd, uint32_t blk, void *buf)
{
    return pread(fd, buf, BLOCK_SIZE, (uint64_t)blk * BLOCK_SIZE) == BLOCK_SIZE
           ? 0 : -1;
}

int write_block(int fd, uint32_t blk, const void *buf)
{
    return pwrite(fd, buf, BLOCK_SIZE, (uint64_t)blk * BLOCK_SIZE) == BLOCK_SIZE
           ? 0 : -1;
}

/* ==================================================================
 *  分配器
 * ================================================================== */

uint32_t alloc_inode(int fd, struct superblock *sb)
{
    /* 从 bit 1 开始扫描（bit 0 保留） */
    for (uint32_t bit = 1; bit < BIT_PER_BLOCK; bit++) {
        if (bitmap_get(fd, (uint64_t)INODE_BMAP_BLK * BLOCK_SIZE, bit) == 0) {
            if (bitmap_set(fd, (uint64_t)INODE_BMAP_BLK * BLOCK_SIZE, bit, 1) < 0)
                return 0;
            sb->free_inode--;
            return bit;
        }
    }
    return 0;   /* 无可用 inode */
}

uint32_t alloc_data_blk(int fd, struct superblock *sb)
{
    /* 从 data_start_blk 开始扫描，跳过元数据区 */
    for (uint32_t bit = sb->data_start_blk; bit < BIT_PER_BLOCK; bit++) {
        if (bitmap_get(fd, (uint64_t)DATA_BMAP_BLK * BLOCK_SIZE, bit) == 0) {
            if (bitmap_set(fd, (uint64_t)DATA_BMAP_BLK * BLOCK_SIZE, bit, 1) < 0)
                return 0;
            sb->free_data_blk--;
            return bit;
        }
    }
    return 0;   /* 无可用数据块 */
}

void free_inode(int fd, struct superblock *sb, uint32_t ino)
{
    bitmap_set(fd, (uint64_t)INODE_BMAP_BLK * BLOCK_SIZE, ino, 0);
    sb->free_inode++;
}

void free_data_blk(int fd, struct superblock *sb, uint32_t blk)
{
    bitmap_set(fd, (uint64_t)DATA_BMAP_BLK * BLOCK_SIZE, blk, 0);
    sb->free_data_blk++;
}

/* ==================================================================
 *  Inode 块映射
 * ================================================================== */

/*
 * inode_get_block — 根据 inode 的块指针树，返回逻辑块号对应的物理块号。
 * 返回 0 表示该逻辑块尚未映射（空洞 / 未写入）。
 *
 * 布局（与 ext2 相同）：
 *   direct[0..11]     → 逻辑块 0 ~ 11
 *   indirect           → 逻辑块 12 ~ 12+1023    (一级间接)
 *   dindirect          → 逻辑块 12+1024 ~ 12+1024+1024*1024-1 (二级间接)
 */
uint32_t inode_get_block(int fd, struct d_inode *din, uint32_t logical_blk)
{
    /* --- 直接块 --- */
    if (logical_blk < 12)
        return din->direct[logical_blk];

    logical_blk -= 12;

    /* --- 一级间接 --- */
    if (logical_blk < PTRS_PER_BLOCK) {
        if (din->indirect == 0) return 0;
        uint32_t ptrs[PTRS_PER_BLOCK];
        if (read_block(fd, din->indirect, ptrs) < 0) return 0;
        return ptrs[logical_blk];
    }

    logical_blk -= PTRS_PER_BLOCK;

    /* --- 二级间接 --- */
    if (logical_blk < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        if (din->dindirect == 0) return 0;
        uint32_t l1[PTRS_PER_BLOCK];
        if (read_block(fd, din->dindirect, l1) < 0) return 0;
        uint32_t idx1 = logical_blk / PTRS_PER_BLOCK;
        if (l1[idx1] == 0) return 0;
        uint32_t l2[PTRS_PER_BLOCK];
        if (read_block(fd, l1[idx1], l2) < 0) return 0;
        return l2[logical_blk % PTRS_PER_BLOCK];
    }

    return 0;   /* 超出寻址范围 */
}

/*
 * inode_alloc_block — 保证逻辑块号到物理块的映射存在。
 * 如果已经分配，直接返回物理块号；否则分配新的数据块并更新映射。
 *
 * 注意：如果分配了间接块本身，需要先将新块清零再写入指针。
 *       调用者之后需要 write_inode 把更新后的 din 写回磁盘。
 */
int inode_alloc_block(int fd, struct superblock *sb,
                      struct d_inode *din, uint32_t logical_blk,
                      uint32_t *out_phys_blk)
{
    /* --- 直接块 --- */
    if (logical_blk < 12) {
        if (din->direct[logical_blk] != 0) {
            *out_phys_blk = din->direct[logical_blk];
            return 0;
        }
        uint32_t blk = alloc_data_blk(fd, sb);
        if (blk == 0) return -ENOSPC;
        din->direct[logical_blk] = blk;
        *out_phys_blk = blk;
        return 0;
    }

    logical_blk -= 12;

    /* --- 一级间接 --- */
    if (logical_blk < PTRS_PER_BLOCK) {
        /* 确保 indirect 块已分配 */
        if (din->indirect == 0) {
            uint32_t iblk = alloc_data_blk(fd, sb);
            if (iblk == 0) return -ENOSPC;
            uint8_t zero[BLOCK_SIZE] = {0};
            write_block(fd, iblk, zero);
            din->indirect = iblk;
        }
        uint32_t ptrs[PTRS_PER_BLOCK];
        if (read_block(fd, din->indirect, ptrs) < 0) return -EIO;

        if (ptrs[logical_blk] != 0) {
            *out_phys_blk = ptrs[logical_blk];
            return 0;
        }
        uint32_t blk = alloc_data_blk(fd, sb);
        if (blk == 0) return -ENOSPC;
        ptrs[logical_blk] = blk;
        if (write_block(fd, din->indirect, ptrs) < 0) return -EIO;
        *out_phys_blk = blk;
        return 0;
    }

    logical_blk -= PTRS_PER_BLOCK;

    /* --- 二级间接 --- */
    if (logical_blk < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        /* 确保 dindirect 块已分配 */
        if (din->dindirect == 0) {
            uint32_t diblk = alloc_data_blk(fd, sb);
            if (diblk == 0) return -ENOSPC;
            uint8_t zero[BLOCK_SIZE] = {0};
            write_block(fd, diblk, zero);
            din->dindirect = diblk;
        }
        uint32_t l1[PTRS_PER_BLOCK];
        if (read_block(fd, din->dindirect, l1) < 0) return -EIO;

        uint32_t idx1 = logical_blk / PTRS_PER_BLOCK;
        /* 确保 L1 间接块已分配 */
        if (l1[idx1] == 0) {
            uint32_t iblk = alloc_data_blk(fd, sb);
            if (iblk == 0) return -ENOSPC;
            uint8_t zero[BLOCK_SIZE] = {0};
            write_block(fd, iblk, zero);
            l1[idx1] = iblk;
            if (write_block(fd, din->dindirect, l1) < 0) return -EIO;
        }

        uint32_t l2[PTRS_PER_BLOCK];
        if (read_block(fd, l1[idx1], l2) < 0) return -EIO;

        uint32_t idx2 = logical_blk % PTRS_PER_BLOCK;
        if (l2[idx2] != 0) {
            *out_phys_blk = l2[idx2];
            return 0;
        }
        uint32_t blk = alloc_data_blk(fd, sb);
        if (blk == 0) return -ENOSPC;
        l2[idx2] = blk;
        if (write_block(fd, l1[idx1], l2) < 0) return -EIO;
        *out_phys_blk = blk;
        return 0;
    }

    return -EFBIG;   /* 超出最大文件大小 */
}

/* ==================================================================
 *  释放 inode 的所有数据块
 * ================================================================== */

int inode_free_all_blocks(int fd, struct superblock *sb, struct d_inode *din)
{
    int freed = 0;

    /* --- 直接块 --- */
    for (int i = 0; i < 12; i++) {
        if (din->direct[i] != 0) {
            free_data_blk(fd, sb, din->direct[i]);
            din->direct[i] = 0;
            freed++;
        }
    }

    /* --- 一级间接 --- */
    if (din->indirect != 0) {
        uint32_t ptrs[PTRS_PER_BLOCK];
        if (read_block(fd, din->indirect, ptrs) == 0) {
            for (int i = 0; i < PTRS_PER_BLOCK; i++) {
                if (ptrs[i] != 0) {
                    free_data_blk(fd, sb, ptrs[i]);
                    freed++;
                }
            }
        }
        free_data_blk(fd, sb, din->indirect);
        din->indirect = 0;
        freed++;
    }

    /* --- 二级间接 --- */
    if (din->dindirect != 0) {
        uint32_t l1[PTRS_PER_BLOCK];
        if (read_block(fd, din->dindirect, l1) == 0) {
            for (int i = 0; i < PTRS_PER_BLOCK; i++) {
                if (l1[i] != 0) {
                    uint32_t l2[PTRS_PER_BLOCK];
                    if (read_block(fd, l1[i], l2) == 0) {
                        for (int j = 0; j < PTRS_PER_BLOCK; j++) {
                            if (l2[j] != 0) {
                                free_data_blk(fd, sb, l2[j]);
                                freed++;
                            }
                        }
                    }
                    free_data_blk(fd, sb, l1[i]);
                    freed++;
                }
            }
        }
        free_data_blk(fd, sb, din->dindirect);
        din->dindirect = 0;
        freed++;
    }

    din->size = 0;
    return freed;
}

/* ==================================================================
 *  目录操作
 * ================================================================== */

/*
 * dir_lookup — 在目录 dir_ino 中查找名为 name 的目录项。
 * 返回 0 并将 inode 号写入 out_ino；未找到返回 -ENOENT。
 */
int dir_lookup(int fd, uint32_t dir_ino, const char *name, uint32_t *out_ino)
{
    struct d_inode din;
    if (read_inode(fd, dir_ino, &din) != INODE_SIZE)
        return -EIO;
    if (!S_ISDIR(din.mode))
        return -ENOTDIR;

    uint32_t nblocks = (din.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (nblocks == 0) nblocks = 1;   /* 至少有一块（含 . 和 ..） */

    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t phys = inode_get_block(fd, &din, lb);
        if (phys == 0) continue;

        char buf[BLOCK_SIZE];
        if (read_block(fd, phys, buf) < 0) return -EIO;

        char *p = buf;
        while (p < buf + BLOCK_SIZE) {
            struct dirent *ent = (struct dirent *)p;
            if (ent->rec_len == 0) break;
            if (ent->ino != 0 && strcmp(ent->name, name) == 0) {
                *out_ino = ent->ino;
                return 0;
            }
            p += ent->rec_len;
        }
    }
    return -ENOENT;
}

/*
 * dir_add_entry — 在目录 dir_ino 末尾（或空闲槽位）插入新目录项。
 * 成功返回 0；目录已满返回 -ENOSPC；名称已存在返回 -EEXIST。
 */
int dir_add_entry(int fd, struct superblock *sb,
                  uint32_t dir_ino, const char *name, uint32_t new_ino)
{
    /* 先检查重名 */
    uint32_t tmp;
    if (dir_lookup(fd, dir_ino, name, &tmp) == 0)
        return -EEXIST;

    struct d_inode din;
    if (read_inode(fd, dir_ino, &din) != INODE_SIZE)
        return -EIO;

    uint32_t nblocks = (din.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (nblocks == 0) nblocks = 1;

    /* 扫描现有块，找空闲槽位（ino==0 表示已删除） */
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t phys = inode_get_block(fd, &din, lb);
        if (phys == 0) continue;

        char buf[BLOCK_SIZE];
        if (read_block(fd, phys, buf) < 0) return -EIO;

        char *p = buf;
        while (p < buf + BLOCK_SIZE) {
            struct dirent *ent = (struct dirent *)p;
            if (ent->rec_len == 0) break;
            if (ent->ino == 0) {
                /* 复用已删除的槽位 */
                ent->ino = new_ino;
                strncpy(ent->name, name, MAX_NAME_LEN);
                ent->name[MAX_NAME_LEN] = '\0';
                ent->rec_len = sizeof(struct dirent);
                if (write_block(fd, phys, buf) < 0) return -EIO;
                /* 目录大小不需要变，因为 rec_len 不变 */
                din.mtime = din.ctime = time(NULL);
                write_inode(fd, dir_ino, &din);
                return 0;
            }
            p += ent->rec_len;
        }
    }

    /* 尝试追加到最后一个块的剩余空间 */
    if (nblocks > 0) {
        uint32_t last_lb   = nblocks - 1;
        uint32_t last_phys = inode_get_block(fd, &din, last_lb);
        if (last_phys != 0) {
            uint32_t used_in_last = din.size - last_lb * BLOCK_SIZE;
            if (used_in_last + sizeof(struct dirent) <= BLOCK_SIZE) {
                /* 块内还有空间，直接追加 */
                char buf[BLOCK_SIZE];
                if (read_block(fd, last_phys, buf) < 0) return -EIO;
                struct dirent *ent = (struct dirent *)(buf + used_in_last);
                ent->ino = new_ino;
                strncpy(ent->name, name, MAX_NAME_LEN);
                ent->name[MAX_NAME_LEN] = '\0';
                ent->rec_len = sizeof(struct dirent);
                if (write_block(fd, last_phys, buf) < 0) return -EIO;
                din.size += sizeof(struct dirent);
                din.mtime = din.ctime = time(NULL);
                if (write_inode(fd, dir_ino, &din) != INODE_SIZE) return -EIO;
                return 0;
            }
        }
    }

    /* 最后一个块也满了，分配新块 */
    uint32_t new_phys;
    int ret = inode_alloc_block(fd, sb, &din, nblocks, &new_phys);
    if (ret < 0) return ret;

    char buf[BLOCK_SIZE];
    memset(buf, 0, BLOCK_SIZE);
    struct dirent *ent = (struct dirent *)buf;
    ent->ino = new_ino;
    strncpy(ent->name, name, MAX_NAME_LEN);
    ent->name[MAX_NAME_LEN] = '\0';
    ent->rec_len = sizeof(struct dirent);

    if (write_block(fd, new_phys, buf) < 0) return -EIO;

    din.size += sizeof(struct dirent);
    din.mtime = din.ctime = time(NULL);
    if (write_inode(fd, dir_ino, &din) != INODE_SIZE) return -EIO;

    /* 还需要同步超级块（分配了数据块） */
    write_superblock(fd, sb);

    return 0;
}

/*
 * dir_del_entry — 在目录 dir_ino 中删除名为 name 的目录项。
 * 实现：将 ino 置 0，保留 rec_len 不变（空间稍后可复用）。
 */
int dir_del_entry(int fd, uint32_t dir_ino, const char *name)
{
    struct d_inode din;
    if (read_inode(fd, dir_ino, &din) != INODE_SIZE)
        return -EIO;

    uint32_t nblocks = (din.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (nblocks == 0) nblocks = 1;

    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t phys = inode_get_block(fd, &din, lb);
        if (phys == 0) continue;

        char buf[BLOCK_SIZE];
        if (read_block(fd, phys, buf) < 0) return -EIO;

        char *p = buf;
        while (p < buf + BLOCK_SIZE) {
            struct dirent *ent = (struct dirent *)p;
            if (ent->rec_len == 0) break;
            if (ent->ino != 0 && strcmp(ent->name, name) == 0) {
                ent->ino = 0;           /* 标记删除 */
                ent->name[0] = '\0';   /* 清空名称 */
                if (write_block(fd, phys, buf) < 0) return -EIO;
                din.mtime = din.ctime = time(NULL);
                write_inode(fd, dir_ino, &din);
                return 0;
            }
            p += ent->rec_len;
        }
    }
    return -ENOENT;
}

/*
 * dir_is_empty — 检查目录是否仅含 "." 和 ".."。
 * 空返回 1，非空返回 0，出错返回负数。
 */
int dir_is_empty(int fd, uint32_t dir_ino)
{
    struct d_inode din;
    if (read_inode(fd, dir_ino, &din) != INODE_SIZE)
        return -EIO;

    uint32_t nblocks = (din.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (nblocks == 0) nblocks = 1;

    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t phys = inode_get_block(fd, &din, lb);
        if (phys == 0) continue;

        char buf[BLOCK_SIZE];
        if (read_block(fd, phys, buf) < 0) return -EIO;

        char *p = buf;
        while (p < buf + BLOCK_SIZE) {
            struct dirent *ent = (struct dirent *)p;
            if (ent->rec_len == 0) break;
            if (ent->ino != 0 &&
                strcmp(ent->name, ".") != 0 &&
                strcmp(ent->name, "..") != 0)
                return 0;   /* 有用户条目，非空 */
            p += ent->rec_len;
        }
    }
    return 1;   /* 只有 . 和 ..，目录为空 */
}

/* ==================================================================
 *  路径解析
 * ================================================================== */

/*
 * path_resolve — 多级路径解析，从根 inode 开始逐级查找。
 * 支持 /a/b/c 格式，路径必须以 '/' 开头。
 */
int path_resolve(int fd, const char *path, uint32_t *out_ino)
{
    if (path[0] != '/')
        return -ENOENT;

    if (strcmp(path, "/") == 0) {
        *out_ino = ROOT_INO;
        return 0;
    }

    const char *p = path + 1;          /* 跳过前导 '/' */
    uint32_t current_ino = ROOT_INO;

    while (*p) {
        /* 提取下一路径分量 */
        char component[MAX_NAME_LEN + 1];
        int i = 0;
        while (*p && *p != '/' && i < MAX_NAME_LEN) {
            component[i++] = *p++;
        }
        component[i] = '\0';
        if (*p == '/') p++;            /* 跳过分隔符 */

        /* 在当前目录中查找 */
        int ret = dir_lookup(fd, current_ino, component, out_ino);
        if (ret < 0) return ret;

        current_ino = *out_ino;
    }

    *out_ino = current_ino;
    return 0;
}

/*
 * path_parent_resolve — 解析父目录 + 最后一个分量名。
 * 例: "/a/b/c" → parent_ino = ino_of("/a/b"), child_name = "c"
 * 特例: "/a"    → parent_ino = ROOT_INO,       child_name = "a"
 */
int path_parent_resolve(int fd, const char *path,
                        uint32_t *parent_ino, char *child_name)
{
    const char *last_slash = strrchr(path, '/');
    if (!last_slash)
        return -EINVAL;

    /* 提取最后分量名 */
    const char *name_start = last_slash + 1;
    strncpy(child_name, name_start, MAX_NAME_LEN);
    child_name[MAX_NAME_LEN] = '\0';

    /* 父路径 */
    if (last_slash == path) {
        /* 父目录就是根 */
        *parent_ino = ROOT_INO;
        return 0;
    }

    char parent_path[4096];
    size_t len = last_slash - path;
    memcpy(parent_path, path, len);
    parent_path[len] = '\0';

    return path_resolve(fd, parent_path, parent_ino);
}
