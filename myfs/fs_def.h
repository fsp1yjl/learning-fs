#ifndef FS_DEF_H
#define FS_DEF_H

#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

// 全局参数
#define BLOCK_SIZE     4096
#define IMG_TOTAL_SIZE (64 * 1024 * 1024) // 64MB镜像
#define INODE_SIZE     256
#define PREFIX_BLOCKS  4 // 0:引导 1:sb 2:inobitmap 3:databitmap

#define BIT_PER_BLOCK  (BLOCK_SIZE * 8)   // 单块位图可标记32768项
#define ROOT_INO       1
#define MAGIC_NUM      0x66757365         // "fuse"魔数

// 超级块，占1整块4096B
struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_inode;
    uint32_t total_block;
    uint32_t inode_table_start_blk; // inode表起始块号
    uint32_t data_start_blk;        // 数据区起始块号
    uint8_t pad[BLOCK_SIZE - 24];
} __attribute__((packed));

// Inode 256字节固定
struct d_inode {
    uint16_t mode;
    uint16_t nlink;
    uint32_t size;
    uint32_t atime, mtime, ctime;
    uint32_t direct[12]; // 直接块指针
    uint8_t pad[INODE_SIZE - (12*4 + 16)];
} __attribute__((packed));

// 目录项，存放于数据块
struct dirent {
    uint32_t ino;
    char name[124];
    uint16_t rec_len;
} __attribute__((packed));

// 工具函数声明
int bitmap_set(int fd, uint32_t blk_off, uint32_t bit, int val);
int bitmap_get(int fd, uint32_t blk_off, uint32_t bit);
ssize_t read_inode(int fd, uint32_t ino, struct d_inode *out);
ssize_t write_inode(int fd, uint32_t ino, struct d_inode *in);

#endif
