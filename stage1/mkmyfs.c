#define _GNU_SOURCE
#include "fs_def.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <errno.h>

int bitmap_set(int fd, uint32_t blk_off, uint32_t bit, int val)
{
    uint8_t buf[BLOCK_SIZE];
    ssize_t r = pread(fd, buf, BLOCK_SIZE, blk_off);
    if (r != BLOCK_SIZE) return -1;

    uint32_t byte_idx = bit / 8;
    uint32_t bit_idx  = bit % 8;
    if (val)
        buf[byte_idx] |= (1 << bit_idx);
    else
        buf[byte_idx] &= ~(1 << bit_idx);

    ssize_t w = pwrite(fd, buf, BLOCK_SIZE, blk_off);
    if (w != BLOCK_SIZE) return -1;
    return 0;
}

int bitmap_get(int fd, uint32_t blk_off, uint32_t bit)
{
    uint8_t buf[BLOCK_SIZE];
    ssize_t r = pread(fd, buf, BLOCK_SIZE, blk_off);
    if (r != BLOCK_SIZE) return -1;

    uint32_t byte_idx = bit / 8;
    uint32_t bit_idx  = bit % 8;
    return (buf[byte_idx] >> bit_idx) & 1;
}

ssize_t read_inode(int fd, uint32_t ino, struct d_inode *out)
{
    uint64_t inode_start = PREFIX_BLOCKS * BLOCK_SIZE;
    uint64_t off = inode_start + (ino - 1) * INODE_SIZE;
    return pread(fd, out, INODE_SIZE, off);
}

ssize_t write_inode(int fd, uint32_t ino, struct d_inode *in)
{
    uint64_t inode_start = PREFIX_BLOCKS * BLOCK_SIZE;
    uint64_t off = inode_start + (ino - 1) * INODE_SIZE;
    return pwrite(fd, in, INODE_SIZE, off);
}

static uint32_t alloc_inode(int fd)
{
    uint32_t bit = 1;
    for (; bit < BIT_PER_BLOCK; bit++) {
        int ret = bitmap_get(fd, 2 * BLOCK_SIZE, bit);
        if (ret == 0) {
            bitmap_set(fd, 2 * BLOCK_SIZE, bit, 1);
            return bit;
        }
    }
    return 0;
}

static uint32_t alloc_data_blk(int fd, uint32_t data_start)
{
    uint32_t bit = data_start;
    for (; bit < BIT_PER_BLOCK; bit++) {
        // 这里对data bitmap的扫描会忽略0-data_start （0-2051）  这些bit，有部分浪费。  
        // 数据位图有 14332 个 bit，但前 2052 个 bit（bit 0 ~ 2051）永远不会被扫描到，浪费了约 6.3% 的位图空间
        int ret = bitmap_get(fd, 3 * BLOCK_SIZE, bit); 
        if (ret == 0) {
            bitmap_set(fd, 3 * BLOCK_SIZE, bit, 1);
            return bit;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s disk.img\n", argv[0]);
        return 1;
    }
    const char *img_path = argv[1];
    int fd = open(img_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open img");
        return 1;
    }
    if (ftruncate(fd, IMG_TOTAL_SIZE) < 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }

    struct superblock sb = {0};
    sb.magic = MAGIC_NUM;
    sb.block_size = BLOCK_SIZE;
    uint32_t total_blk = IMG_TOTAL_SIZE / BLOCK_SIZE;
    sb.total_block = total_blk;
    sb.total_inode = BIT_PER_BLOCK;
    sb.inode_table_start_blk = PREFIX_BLOCKS;
    sb.data_start_blk = PREFIX_BLOCKS + (BIT_PER_BLOCK * INODE_SIZE / BLOCK_SIZE);

    ssize_t wsb = pwrite(fd, &sb, sizeof(sb), 1 * BLOCK_SIZE);  // write superblock to block 1
    if (wsb != sizeof(sb)) { perror("write sb"); close(fd); return 1; }

    uint32_t root_ino = ROOT_INO;  // root inode index is 1
    // inode bitmap is in block 2, set inode bitmap for root directory inode index 1 to 1, representing that the inode is used
    bitmap_set(fd, 2 * BLOCK_SIZE, root_ino, 1); 
    struct d_inode root_in = {0};      // 初始化根目录 inode
    time_t now = time(NULL);
    root_in.mode = 040755;
    root_in.nlink = 2;  // 根目录下存在 .（指向自身）、..（父目录，根的父是自己），所以链接数固定为 2；普通文件 nlink=1 
    root_in.atime = root_in.mtime = root_in.ctime = now;
    uint32_t root_blk = alloc_data_blk(fd, sb.data_start_blk);   // 返回分配的第一个数据块号，作为根目录的数据块 
    root_in.direct[0] = root_blk;
    root_in.size = sizeof(struct dirent) * 2;  // 根目录下有两个目录项：. 和 ..，所以 size=2*dirent_size
    write_inode(fd, root_ino, &root_in);  // 将根目录的 inode 写入 inode 表

    char dir_buf[BLOCK_SIZE] = {0};  // dir_buf 用于存放根目录的目录项数据，大小为 BLOCK_SIZE
    struct dirent *dot = (struct dirent *)dir_buf;
    dot->ino = root_ino;
    strcpy(dot->name, ".");
    dot->rec_len = sizeof(struct dirent); // 目录项的长度固定为 sizeof(struct dirent)

    struct dirent *dotdot = (struct dirent *)(dir_buf + sizeof(struct dirent));
    dotdot->ino = root_ino;
    strcpy(dotdot->name, "..");
    dotdot->rec_len = sizeof(struct dirent);

    ssize_t wroot = pwrite(fd, dir_buf, BLOCK_SIZE, root_blk * BLOCK_SIZE); // 将根目录的目录项数据写入根目录对应的数据块
    if (wroot != BLOCK_SIZE) { perror("write root block"); close(fd); return 1; }

    uint32_t file_ino = alloc_inode(fd);  // 分配一个新的 inode 号，用于创建 test.txt 文件
    struct d_inode file_in = {0};
    const char content[] = "Hello MiniFS!\nThis is read-only FUSE demo.\n";
    uint32_t content_len = strlen(content);
    uint32_t file_blk = alloc_data_blk(fd, sb.data_start_blk);   // 分配一个新的数据块号，用于存放 test.txt 文件的内容

    ssize_t wfile = pwrite(fd, content, content_len, file_blk * BLOCK_SIZE);  // 将 test.txt 文件的内容写入分配的数据块
    if (wfile != content_len) { perror("write file data"); close(fd); return 1; }

    file_in.mode = 0100644;
    file_in.nlink = 1;
    file_in.size = content_len;
    file_in.atime = file_in.mtime = file_in.ctime = now;
    file_in.direct[0] = file_blk;
    write_inode(fd, file_ino, &file_in);  // 将 test.txt 文件的 inode 写入 inode 表

    struct dirent *txt_ent = (struct dirent *)(dir_buf + sizeof(struct dirent)*2);
    txt_ent->ino = file_ino;
    strcpy(txt_ent->name, "test.txt");   // 将 test.txt 文件的目录项添加到根目录的目录项数据中
    txt_ent->rec_len = sizeof(struct dirent);  
    root_in.size += sizeof(struct dirent);  // 更新根目录的 size，表示根目录下的目录项数量增加了一个
    root_in.mtime = root_in.ctime = time(NULL);

    ssize_t wdir = pwrite(fd, dir_buf, BLOCK_SIZE, root_blk * BLOCK_SIZE);  // 将更新后的根目录的目录项数据写入根目录对应的数据块
    if (wdir != BLOCK_SIZE) { perror("write dirent"); close(fd); return 1; }
    write_inode(fd, root_ino, &root_in);  // 将更新后的根目录 inode 写入 inode 表

    close(fd);
    printf("Format success: %s\n", img_path);
    printf("Root ino=1, test.txt ino=%u\n", file_ino);
    return 0;
}
