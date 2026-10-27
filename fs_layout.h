#ifndef FS_LAYOUT_H
#define FS_LAYOUT_H

#include <stdint.h>
#include <sys/types.h>

#define BLOCK_SIZE         4096
#define NUM_BLOCKS         1024
#define NUM_INODES         128
#define MAGIC_NUM          0x5553464D
#define DIRECT_BLOCKS      12
#define MAX_FILENAME_LEN   56

/* disk block offsets (inode table = 128 * 512B = 16 blocks) */
#define SUPERBLOCK_BLOCK   0
#define INODE_BITMAP_BLOCK 1
#define DATA_BITMAP_BLOCK  2
#define INODE_TABLE_BLOCK  3
#define DATA_START_BLOCK   19

struct superblock {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t block_size;
};

/* 512 bytes per inode -> 8 inodes per 4KB block */
struct inode {
    uint32_t mode;
    uint32_t size;
    uint32_t blocks[DIRECT_BLOCKS];
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint8_t  padding[476];
};

/* 64 bytes per dir entry -> 64 entries per 4KB block
 * valid: 0 = free, 1 = active, 2 = soft-deleted
 */
struct dir_entry {
    char     name[MAX_FILENAME_LEN];
    uint32_t inode_no;
    uint8_t  valid;
    uint8_t  padding[3];
};

#endif