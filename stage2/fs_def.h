#ifndef FS_DEF_H
#define FS_DEF_H

#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

/* ================================================================
 *  全局参数
 * ================================================================ */
#define BLOCK_SIZE      4096
#define IMG_TOTAL_SIZE  (64 * 1024 * 1024)   /* 64MB 镜像 */
#define INODE_SIZE      256                   /* Inode 固定大小 */
#define PREFIX_BLOCKS   4                     /* 0:引导 1:sb 2:inobitmap 3:databitmap */

#define BIT_PER_BLOCK   (BLOCK_SIZE * 8)      /* 单块位图可标记 32768 项 */
#define ROOT_INO        1
#define MAGIC_NUM       0x66757365            /* "fuse" 魔数 */

/* 间接块指针参数：每块可放 4096/4 = 1024 个指针 */
#define PTRS_PER_BLOCK  (BLOCK_SIZE / sizeof(uint32_t))

/* 位图所在块号（用于计算字节偏移） */
#define INODE_BMAP_BLK  2
#define DATA_BMAP_BLK   3

/* ================================================================
 *  超级块 — 占 1 整块 4096B
 * ================================================================ */
struct superblock {
    uint32_t magic;               /* 0x66757365 ("fuse") */
    uint32_t block_size;          /* 4096 */
    uint32_t total_inode;         /* 最大 inode 数 = 32768 */
    uint32_t total_block;         /* 总块数 = 16384 */
    uint32_t inode_table_start_blk; /* inode 表起始块号 = 4 */
    uint32_t data_start_blk;      /* 数据区起始块号 = 2052 */
    uint32_t free_inode;          /* 空闲 inode 计数 */
    uint32_t free_data_blk;       /* 空闲数据块计数 */
    uint8_t  pad[BLOCK_SIZE - 32];
} __attribute__((packed));

/* ================================================================
 *  Inode — 256 字节固定
 *  对标 ext2：12 直接 + 1 一级间接 + 1 二级间接
 *  寻址能力：12 + 1024 + 1024×1024 = 1,059,612 块 ≈ 4 GB
 * ================================================================ */
struct d_inode {
    uint16_t mode;               /* 文件类型 + 权限 */
    uint16_t nlink;              /* 硬链接数 */
    uint32_t size;               /* 文件/目录大小（字节） */
    uint32_t atime, mtime, ctime; /* 访问/修改/状态变更时间 */
    uint32_t direct[12];         /* 直接块指针 */
    uint32_t indirect;           /* 一级间接块指针 */
    uint32_t dindirect;          /* 二级间接块指针 */
    uint8_t  pad[INODE_SIZE - (14 * 4 + 20)];  /* 180 字节填充 */
} __attribute__((packed));

/* ================================================================
 *  目录项 — 存放于数据块
 *  固定 130 字节，每块 4096/130 = 31 条
 * ================================================================ */
struct dirent {
    uint32_t ino;                /* Inode 号 */
    char     name[124];          /* 文件名（最长 123 字节 + '\0'） */
    uint16_t rec_len;            /* 本条目长度 = sizeof(struct dirent) = 130 */
} __attribute__((packed));

#define DIRENTS_PER_BLK  (BLOCK_SIZE / sizeof(struct dirent))  /* 31 */
#define MAX_NAME_LEN     123

#endif /* FS_DEF_H */
