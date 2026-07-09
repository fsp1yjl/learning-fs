#define _GNU_SOURCE
#define FUSE_USE_VERSION 31
#include <time.h>
#include <fuse3/fuse.h>
#include "fs_def.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

static int img_fd;
static struct superblock sb_cache;

// 复制工具函数，解决undefined reference
int bitmap_set(int fd, uint32_t blk_off, uint32_t bit, int val)
{
    uint8_t buf[BLOCK_SIZE];
    ssize_t r = pread(fd, buf, BLOCK_SIZE, blk_off);
    if (r != BLOCK_SIZE) return -1;
    uint32_t byte_idx = bit / 8;
    uint32_t bit_idx  = bit % 8;
    if (val) buf[byte_idx] |= (1 << bit_idx);
    else buf[byte_idx] &= ~(1 << bit_idx);
    ssize_t w = pwrite(fd, buf, BLOCK_SIZE, blk_off);
    return (w == BLOCK_SIZE) ? 0 : -1;
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

static int path_to_ino(const char *path, uint32_t *out_ino)
{
    if (strcmp(path, "/") == 0) {
        *out_ino = ROOT_INO;
        return 0;
    }
    if (path[0] != '/')
        return -ENOENT;
    const char *name = path + 1;
    struct d_inode root;
    ssize_t ri = read_inode(img_fd, ROOT_INO, &root); // 读取根目录inode信息，放入root变量
    if (ri != INODE_SIZE) return -EIO;

    uint64_t data_off = root.direct[0] * BLOCK_SIZE;  // 根目录数据块偏移
    char buf[BLOCK_SIZE] = {0};
    ssize_t pr = pread(img_fd, buf, BLOCK_SIZE, data_off);  // 读取根目录数据块内容，放入buf变量
    if (pr < 0) return -EIO;

    char *p = buf;
    while (p < buf + BLOCK_SIZE) {
        // 遍历目录项，查找匹配的文件名,如果找到则返回对应的inode号
        struct dirent *ent = (struct dirent *)p;
        if (ent->rec_len == 0) break;
        if (strcmp(ent->name, name) == 0) {
            *out_ino = ent->ino;
            return 0;
        }
        p += ent->rec_len;
    }
    return -ENOENT;
}

static int fs_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    // 获取文件属性,先找到对应的inode号，再读取inode信息，最后填充stat结构体
    (void)fi;
    uint32_t ino;
    int ret = path_to_ino(path, &ino);
    if (ret != 0) return ret;

    struct d_inode din;
    ssize_t ri = read_inode(img_fd, ino, &din);
    if (ri != INODE_SIZE) return -EIO;

    memset(st, 0, sizeof(*st));
    st->st_ino = ino;
    st->st_mode = din.mode;
    st->st_nlink = din.nlink;
    st->st_size = din.size;
    st->st_atime = din.atime;
    st->st_mtime = din.mtime;
    st->st_ctime = din.ctime;
    st->st_blksize = BLOCK_SIZE;
    st->st_blocks = (din.size + BLOCK_SIZE - 1) / BLOCK_SIZE * (BLOCK_SIZE / 512);
    return 0;
}

static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    // step1 : 获取目录的inode号
    (void)offset; (void)fi; (void)flags;
    uint32_t dir_ino;
    int ret = path_to_ino(path, &dir_ino);
    if (ret != 0) return ret;

    // step2: 读取目录的inode信息，检查是否为目录类型
    struct d_inode din;
    ssize_t ri = read_inode(img_fd, dir_ino, &din);
    if (ri != INODE_SIZE) return -EIO;
    if (!S_ISDIR(din.mode))
        return -ENOTDIR;

    // step3: 读取目录数据块，用户遍历目录项
    uint64_t data_off = din.direct[0] * BLOCK_SIZE;
    char block_buf[BLOCK_SIZE] = {0};
    ssize_t pr = pread(img_fd, block_buf, BLOCK_SIZE, data_off);
    if (pr < 0) return -EIO;

    char *p = block_buf;
    while (p < block_buf + BLOCK_SIZE) {
        // step4: 遍历目录项，获取子文件的dirent结构体，调用filler函数将文件名填充到buf中
        struct dirent *ent = (struct dirent *)p; //这里依赖于dirent结构体的定义，包含ino、name和rec_len字段
        if (ent->rec_len == 0) break;
        filler(buf, ent->name, NULL, 0, 0);
        p += ent->rec_len;
    }
    return 0;
}

// 打开文件，检查文件类型和权限，返回文件inode号作为文件句柄
static int fs_open(const char *path, struct fuse_file_info *fi)
{
    uint32_t ino; // ino存放文件对应的inode号
    int ret = path_to_ino(path, &ino);
    if (ret != 0) return ret;
    struct d_inode din;
    ssize_t ri = read_inode(img_fd, ino, &din);
    if (ri != INODE_SIZE) return -EIO;
    if (!S_ISREG(din.mode))
        return -EISDIR;
    if ((fi->flags & O_WRONLY) || (fi->flags & O_RDWR))
        return -EROFS;
    fi->fh = ino;
    return 0;
}


// fs_read直接使用fs_open返回的文件句柄（inode号）来读取文件内容。
//它首先读取对应的inode信息，然后根据偏移量和大小计算需要读取的数据块和偏移量，最后使用pread从磁盘镜像中读取数据到用户缓冲区中。
static int fs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    uint32_t ino = fi->fh;
    struct d_inode din;
    ssize_t ri = read_inode(img_fd, ino, &din);
    if (ri != INODE_SIZE) return -EIO;

    if (offset >= din.size)
        return 0;
    size_t remain = din.size - offset;
    if (size > remain) size = remain;

    uint32_t blk_id = offset / BLOCK_SIZE;
    off_t blk_off = offset % BLOCK_SIZE;
    uint64_t data_start = din.direct[blk_id] * BLOCK_SIZE + blk_off;

    ssize_t pr = pread(img_fd, buf, size, data_start);
    if (pr < 0) return -EIO;
    return pr;
}

static struct fuse_operations fs_ops = {
    .getattr = fs_getattr,
    .readdir = fs_readdir,
    .open    = fs_open,
    .read    = fs_read,
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s disk.img mountpoint [fuse args]\n", argv[0]);
        return 1;
    }
    img_fd = open(argv[1], O_RDONLY);
    if (img_fd < 0) {
        perror("open img");
        return 1;
    }
    ssize_t pr = pread(img_fd, &sb_cache, sizeof(sb_cache), 1 * BLOCK_SIZE);
    if (pr != sizeof(sb_cache)) {
        perror("read superblock");
        close(img_fd);
        return 1;
    }
    if (sb_cache.magic != MAGIC_NUM) {
        fprintf(stderr, "Not MiniFS image!\n");
        close(img_fd);
        return 1;
    }

    struct fuse_args args = FUSE_ARGS_INIT(argc - 1, argv + 1);
    int ret = fuse_main_real(args.argc, args.argv, &fs_ops, sizeof(fs_ops), NULL);
    fuse_opt_free_args(&args);
    close(img_fd);
    return ret;
}
