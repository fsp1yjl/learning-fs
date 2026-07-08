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

    ssize_t wsb = pwrite(fd, &sb, sizeof(sb), 1 * BLOCK_SIZE);
    if (wsb != sizeof(sb)) { perror("write sb"); close(fd); return 1; }

    uint32_t root_ino = ROOT_INO;
    bitmap_set(fd, 2 * BLOCK_SIZE, root_ino, 1);
    struct d_inode root_in = {0};
    time_t now = time(NULL);
    root_in.mode = 040755;
    root_in.nlink = 2;
    root_in.atime = root_in.mtime = root_in.ctime = now;
    uint32_t root_blk = alloc_data_blk(fd, sb.data_start_blk);
    root_in.direct[0] = root_blk;
    root_in.size = sizeof(struct dirent) * 2;
    write_inode(fd, root_ino, &root_in);

    char dir_buf[BLOCK_SIZE] = {0};
    struct dirent *dot = (struct dirent *)dir_buf;
    dot->ino = root_ino;
    strcpy(dot->name, ".");
    dot->rec_len = sizeof(struct dirent);

    struct dirent *dotdot = (struct dirent *)(dir_buf + sizeof(struct dirent));
    dotdot->ino = root_ino;
    strcpy(dotdot->name, "..");
    dotdot->rec_len = sizeof(struct dirent);

    ssize_t wroot = pwrite(fd, dir_buf, BLOCK_SIZE, root_blk * BLOCK_SIZE);
    if (wroot != BLOCK_SIZE) { perror("write root block"); close(fd); return 1; }

    uint32_t file_ino = alloc_inode(fd);
    struct d_inode file_in = {0};
    const char content[] = "Hello MiniFS!\nThis is read-only FUSE demo.\n";
    uint32_t content_len = strlen(content);
    uint32_t file_blk = alloc_data_blk(fd, sb.data_start_blk);

    ssize_t wfile = pwrite(fd, content, content_len, file_blk * BLOCK_SIZE);
    if (wfile != content_len) { perror("write file data"); close(fd); return 1; }

    file_in.mode = 0100644;
    file_in.nlink = 1;
    file_in.size = content_len;
    file_in.atime = file_in.mtime = file_in.ctime = now;
    file_in.direct[0] = file_blk;
    write_inode(fd, file_ino, &file_in);

    struct dirent *txt_ent = (struct dirent *)(dir_buf + sizeof(struct dirent)*2);
    txt_ent->ino = file_ino;
    strcpy(txt_ent->name, "test.txt");
    txt_ent->rec_len = sizeof(struct dirent);
    root_in.size += sizeof(struct dirent);

    ssize_t wdir = pwrite(fd, dir_buf, BLOCK_SIZE, root_blk * BLOCK_SIZE);
    if (wdir != BLOCK_SIZE) { perror("write dirent"); close(fd); return 1; }
    write_inode(fd, root_ino, &root_in);

    close(fd);
    printf("Format success: %s\n", img_path);
    printf("Root ino=1, test.txt ino=%u\n", file_ino);
    return 0;
}
