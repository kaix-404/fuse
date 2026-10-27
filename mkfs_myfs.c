#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "fs_layout.h"

#define blk_off(b) ((off_t)(b) * BLOCK_SIZE)

int main(void) {
    int fd = open("disk.img", O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open disk.img");
        return 1;
    }

    if (ftruncate(fd, NUM_BLOCKS * BLOCK_SIZE) < 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }

    /* reserve inode 0 and first data block for "/" */
    struct superblock sb = {
        .magic        = MAGIC_NUM,
        .total_blocks = NUM_BLOCKS,
        .total_inodes = NUM_INODES,
        .free_blocks  = (NUM_BLOCKS - DATA_START_BLOCK) - 1,
        .free_inodes  = NUM_INODES - 1,
        .block_size   = BLOCK_SIZE
    };
    pwrite(fd, &sb, sizeof(sb), blk_off(SUPERBLOCK_BLOCK));

    uint8_t bm[BLOCK_SIZE];
    memset(bm, 0, sizeof(bm));
    bm[0] = 0x01; /* bit 0 claimed for root */
    pwrite(fd, bm, BLOCK_SIZE, blk_off(INODE_BITMAP_BLOCK));
    pwrite(fd, bm, BLOCK_SIZE, blk_off(DATA_BITMAP_BLOCK));

    time_t now = time(NULL);
    struct inode root;
    memset(&root, 0, sizeof(root));
    root.mode      = S_IFDIR | 0755;
    root.size      = BLOCK_SIZE;
    root.blocks[0] = DATA_START_BLOCK;
    root.atime     = now;
    root.mtime     = now;
    root.ctime     = now;
    pwrite(fd, &root, sizeof(root), blk_off(INODE_TABLE_BLOCK));

    struct dir_entry de[BLOCK_SIZE / sizeof(struct dir_entry)];
    memset(de, 0, sizeof(de));

    strcpy(de[0].name, ".");
    de[0].inode_no = 0;
    de[0].valid    = 1;

    strcpy(de[1].name, "..");
    de[1].inode_no = 0;
    de[1].valid    = 1;

    pwrite(fd, de, sizeof(de), blk_off(DATA_START_BLOCK));

    close(fd);
    printf("Successfully initialized disk.img (4MB, 1024 blocks)\n");
    return 0;
}