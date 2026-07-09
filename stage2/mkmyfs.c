#define _GNU_SOURCE
#include "fs_def.h"
#include "fs_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s disk.img\n", argv[0]);
        return 1;
    }
    const char *img_path = argv[1];
    int fd = open(img_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open img"); return 1; }
    if (ftruncate(fd, IMG_TOTAL_SIZE) < 0) {
        perror("ftruncate"); close(fd); return 1;
    }

    /* ---- 计算布局 ---- */
    uint32_t total_blk      = IMG_TOTAL_SIZE / BLOCK_SIZE;     /* 16384 */
    uint32_t data_start_blk = PREFIX_BLOCKS
                            + (BIT_PER_BLOCK * INODE_SIZE / BLOCK_SIZE);  /* 2052 */

    /* step1 ---- 写入超级块 ---- */
    struct superblock sb = {0};
    sb.magic              = MAGIC_NUM;
    sb.block_size         = BLOCK_SIZE;
    sb.total_inode        = BIT_PER_BLOCK;    /* 32768 */
    sb.total_block        = total_blk;        /* 16384 */
    sb.inode_table_start_blk = PREFIX_BLOCKS;   /* 4 */
    sb.data_start_blk     = data_start_blk;     /* 2052 */
    sb.free_inode         = BIT_PER_BLOCK - 1;  /* bit 0 保留 */
    sb.free_data_blk      = total_blk - data_start_blk;

    if (write_superblock(fd, &sb) < 0) {
        perror("write superblock"); close(fd); return 1;
    }

    /* ---- 创建根目录 (inode 1) ---- */
    time_t now = time(NULL);

    /* step2 ---- 标记 inode 1 已用,对应根目录的inode （inode bitmap） ---- */
    bitmap_set(fd, (uint64_t)INODE_BMAP_BLK * BLOCK_SIZE, ROOT_INO, 1);
    sb.free_inode--;

    struct d_inode root_in = {0};
    root_in.mode  = S_IFDIR | 0755;
    root_in.nlink = 2;             /* . 和 .. */
    root_in.atime = root_in.mtime = root_in.ctime = now;

    /* step3 ---- 分配根目录的数据块，更新根目录对应的数据位图（data block bitmap） ---- */
    uint32_t root_blk = alloc_data_blk(fd, &sb);
    if (root_blk == 0) {
        fprintf(stderr, "Failed to alloc root data block\n");
        close(fd); return 1;
    }
    root_in.direct[0] = root_blk;
    root_in.size = sizeof(struct dirent) * 2;  /* . 和 .. */

    /* step4 ---- 写入根目录数据到数据块："." 和 ".." ---- */
    char dir_buf[BLOCK_SIZE] = {0};
    struct dirent *dot    = (struct dirent *)dir_buf;
    dot->ino      = ROOT_INO;
    strcpy(dot->name, ".");
    dot->rec_len  = sizeof(struct dirent);

    struct dirent *dotdot = (struct dirent *)(dir_buf + sizeof(struct dirent));
    dotdot->ino    = ROOT_INO;
    strcpy(dotdot->name, "..");
    dotdot->rec_len = sizeof(struct dirent);

    if (write_block(fd, root_blk, dir_buf) < 0) {
        perror("write root block"); close(fd); return 1;
    }

    /* step5 ---- 写入根目录的 inode ---- */
    write_inode(fd, ROOT_INO, &root_in);

    /* step6 ---- 同步超级块（分配了根数据块） ---- */
    // 每次分配 inode 或数据块后，都需要同步超级块，更新 free_inode 和 free_data_blk 计数。
    if (write_superblock(fd, &sb) < 0) {
        perror("write superblock 2"); close(fd); return 1;
    }

    close(fd);
    printf("Format success: %s\n", img_path);
    printf("  total_blk=%u  data_start=%u  free_inode=%u  free_data=%u\n",
           sb.total_block, sb.data_start_blk, sb.free_inode, sb.free_data_blk);
    printf("  Root ino=%u  root_data_blk=%u\n", ROOT_INO, root_blk);
    return 0;
}
