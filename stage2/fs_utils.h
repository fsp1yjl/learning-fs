#ifndef FS_UTILS_H
#define FS_UTILS_H

#include "fs_def.h"

/* ----------------------------------------------------------------
 *  位图操作
 * ---------------------------------------------------------------- */
int bitmap_set(int fd, uint64_t byte_off, uint32_t bit, int val);
int bitmap_get(int fd, uint64_t byte_off, uint32_t bit);

/* ----------------------------------------------------------------
 *  Inode 读写
 * ---------------------------------------------------------------- */
ssize_t read_inode(int fd, uint32_t ino, struct d_inode *out);
ssize_t write_inode(int fd, uint32_t ino, struct d_inode *in);

/* ----------------------------------------------------------------
 *  超级块读写
 * ---------------------------------------------------------------- */
int read_superblock(int fd, struct superblock *sb);
int write_superblock(int fd, const struct superblock *sb);

/* ----------------------------------------------------------------
 *  数据块读写
 * ---------------------------------------------------------------- */
int read_block(int fd, uint32_t blk, void *buf);
int write_block(int fd, uint32_t blk, const void *buf);

/* ----------------------------------------------------------------
 *  分配 / 释放
 *  alloc_data_blk / free_data_blk 需要 data_start 以跳过元数据区
 * ---------------------------------------------------------------- */
uint32_t alloc_inode(int fd, struct superblock *sb);
uint32_t alloc_data_blk(int fd, struct superblock *sb);
void free_inode(int fd, struct superblock *sb, uint32_t ino);
void free_data_blk(int fd, struct superblock *sb, uint32_t blk);

/* ----------------------------------------------------------------
 *  Inode 块映射 — 逻辑块号 → 物理块号
 *  inode_get_block : 查询映射，返回 0 表示该逻辑块未分配
 *  inode_alloc_block: 保证映射存在（若未分配则分配），返回物理块号
 * ---------------------------------------------------------------- */
uint32_t inode_get_block(int fd, struct d_inode *din, uint32_t logical_blk);
int inode_alloc_block(int fd, struct superblock *sb,
                      struct d_inode *din, uint32_t logical_blk,
                      uint32_t *out_phys_blk);

/* ----------------------------------------------------------------
 *  释放 inode 的所有数据块（用于 truncate 到 0 或 unlink）
 *  返回释放的块数
 * ---------------------------------------------------------------- */
int inode_free_all_blocks(int fd, struct superblock *sb, struct d_inode *din);

/* ----------------------------------------------------------------
 *  目录操作
 * ---------------------------------------------------------------- */

/* 在目录 dir_ino 中查找 name，成功返回 0 并将 inode 号写入 out_ino */
int dir_lookup(int fd, uint32_t dir_ino, const char *name, uint32_t *out_ino);

/* 在目录 dir_ino 中添加一条 name → new_ino 的目录项 */
int dir_add_entry(int fd, struct superblock *sb,
                  uint32_t dir_ino, const char *name, uint32_t new_ino);

/* 在目录 dir_ino 中删除 name 对应的目录项（置 ino=0 标记删除） */
int dir_del_entry(int fd, uint32_t dir_ino, const char *name);

/* 检查目录是否为空（仅含 . 和 ..） */
int dir_is_empty(int fd, uint32_t dir_ino);

/* ----------------------------------------------------------------
 *  路径解析
 * ---------------------------------------------------------------- */

/* 多级路径解析：/a/b/c → 逐级查找，返回 0 和最终 inode 号 */
int path_resolve(int fd, const char *path, uint32_t *out_ino);

/* 解析父目录 + 最后分量名：/a/b/c → parent_ino=/a/b, child_name="c" */
int path_parent_resolve(int fd, const char *path,
                        uint32_t *parent_ino, char *child_name);

#endif /* FS_UTILS_H */
