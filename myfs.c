#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <limits.h>
#include <time.h>
#include "fs_layout.h"

#define DIR_CAP (BLOCK_SIZE / sizeof(struct dir_entry))
#define DATA_CAP (NUM_BLOCKS - DATA_START_BLOCK)

static void *disk = NULL;
static char disk_path[PATH_MAX];
static char ctl_buf[512];

static inline struct superblock *get_sb(void) {
    return (struct superblock *)disk;
}

static inline uint8_t *get_inode_bm(void) {
    return (uint8_t *)disk + (INODE_BITMAP_BLOCK * BLOCK_SIZE);
}

static inline uint8_t *get_data_bm(void) {
    return (uint8_t *)disk + (DATA_BITMAP_BLOCK * BLOCK_SIZE);
}

static inline struct inode *get_inode(int idx) {
    return (struct inode *)((uint8_t *)disk + (INODE_TABLE_BLOCK * BLOCK_SIZE) + (idx * sizeof(struct inode)));
}

static inline void *get_block(int bno) {
    return (uint8_t *)disk + (bno * BLOCK_SIZE);
}

/* bitmap helpers */
static inline int test_bit(uint8_t *bm, int i) {
    return (bm[i >> 3] >> (i & 7)) & 1;
}

static int alloc_bit(uint8_t *bm, int limit) {
    for (int i = 0; i < limit; i++) {
        if (!test_bit(bm, i)) {
            bm[i >> 3] |= (1 << (i & 7));
            return i;
        }
    }
    return -ENOSPC;
}

static void free_bit(uint8_t *bm, int i) {
    bm[i >> 3] &= ~(1 << (i & 7));
}

static void purge_inode(int ino) {
    struct inode *node = get_inode(ino);
    for (int i = 0; i < DIRECT_BLOCKS; i++) {
        if (node->blocks[i]) {
            free_bit(get_data_bm(), node->blocks[i] - DATA_START_BLOCK);
            node->blocks[i] = 0;
            get_sb()->free_blocks++;
        }
    }
    free_bit(get_inode_bm(), ino);
    get_sb()->free_inodes++;
}

/* walk path from root inode (0) down to target */
static int resolve_path(const char *path) {
    if (strcmp(path, "/") == 0) return 0;

    char buf[PATH_MAX];
    strncpy(buf, path, PATH_MAX - 1);
    buf[PATH_MAX - 1] = '\0';

    int cur = 0;
    char *save = NULL;
    char *tok = strtok_r(buf, "/", &save);

    while (tok) {
        struct inode *node = get_inode(cur);
        if (!(node->mode & S_IFDIR)) return -ENOTDIR;

        struct dir_entry *de = (struct dir_entry *)get_block(node->blocks[0]);
        int next = -1;

        for (int i = 0; i < (int)DIR_CAP; i++) {
            if (de[i].valid == 1 && strcmp(de[i].name, tok) == 0) {
                next = de[i].inode_no;
                break;
            }
        }
        if (next < 0) return -ENOENT;
        cur = next;
        tok = strtok_r(NULL, "/", &save);
    }
    return cur;
}

static int resolve_parent(const char *path, char *leaf) {
    char buf[PATH_MAX];
    strncpy(buf, path, PATH_MAX - 1);
    buf[PATH_MAX - 1] = '\0';

    char *slash = strrchr(buf, '/');
    if (!slash) return -EINVAL;

    strncpy(leaf, slash + 1, MAX_FILENAME_LEN - 1);
    leaf[MAX_FILENAME_LEN - 1] = '\0';

    if (slash == buf) return 0;
    *slash = '\0';
    return resolve_path(buf);
}

static void *myfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    /* disable atomic o_trunc so kernel routes '>' through .truncate/.open */
    conn->want &= ~FUSE_CAP_ATOMIC_O_TRUNC;

    int fd = open(disk_path, O_RDWR);
    if (fd < 0) {
        perror("open disk.img");
        exit(1);
    }

    disk = mmap(NULL, NUM_BLOCKS * BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (disk == MAP_FAILED) {
        perror("mmap");
        close(fd);
        exit(1);
    }
    close(fd);

    cfg->kernel_cache = 0;
    return NULL;
}

static int is_virtual_file(const char *path) {
    return (strcmp(path, "/.diskstat") == 0 ||
            strcmp(path, "/.recover") == 0  ||
            strcmp(path, "/.purge") == 0);
}

static int myfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));

    if (is_virtual_file(path)) {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = 1024;
        stbuf->st_mtime = time(NULL);
        return 0;
    }

    int ino = resolve_path(path);
    if (ino < 0) return ino;

    struct inode *in = get_inode(ino);
    stbuf->st_mode = in->mode;
    stbuf->st_nlink = (in->mode & S_IFDIR) ? 2 : 1;
    stbuf->st_size = in->size;
    stbuf->st_atime = in->atime;
    stbuf->st_mtime = in->mtime;
    stbuf->st_ctime = in->ctime;
    return 0;
}

static int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;

    int ino = resolve_path(path);
    if (ino < 0) return ino;

    struct inode *dir = get_inode(ino);
    if (!(dir->mode & S_IFDIR)) return -ENOTDIR;

    struct dir_entry *de = (struct dir_entry *)get_block(dir->blocks[0]);
    for (int i = 0; i < (int)DIR_CAP; i++) {
        if (de[i].valid == 1)
            filler(buf, de[i].name, NULL, 0, 0);
    }

    if (ino == 0)
        filler(buf, ".diskstat", NULL, 0, 0);

    return 0;
}

static int myfs_mkdir(const char *path, mode_t mode) {
    if (resolve_path(path) >= 0) return -EEXIST;

    char name[MAX_FILENAME_LEN];
    int pino = resolve_parent(path, name);
    if (pino < 0) return pino;

    struct inode *parent = get_inode(pino);
    if (!(parent->mode & S_IFDIR)) return -ENOTDIR;

    int ino = alloc_bit(get_inode_bm(), NUM_INODES);
    if (ino < 0) return ino;

    int d_idx = alloc_bit(get_data_bm(), DATA_CAP);
    if (d_idx < 0) {
        free_bit(get_inode_bm(), ino);
        return d_idx;
    }

    struct inode *node = get_inode(ino);
    memset(node, 0, sizeof(struct inode));
    node->mode = S_IFDIR | (mode & 0777);
    node->size = BLOCK_SIZE;
    node->blocks[0] = DATA_START_BLOCK + d_idx;
    node->atime = node->mtime = node->ctime = time(NULL);

    struct dir_entry *de = (struct dir_entry *)get_block(node->blocks[0]);
    memset(de, 0, BLOCK_SIZE);

    strcpy(de[0].name, ".");
    de[0].inode_no = ino;
    de[0].valid = 1;

    strcpy(de[1].name, "..");
    de[1].inode_no = pino;
    de[1].valid = 1;

    struct dir_entry *pde = (struct dir_entry *)get_block(parent->blocks[0]);
    for (int i = 0; i < (int)DIR_CAP; i++) {
        if (pde[i].valid == 0) {
            strncpy(pde[i].name, name, MAX_FILENAME_LEN - 1);
            pde[i].name[MAX_FILENAME_LEN - 1] = '\0';
            pde[i].inode_no = ino;
            pde[i].valid = 1;
            get_sb()->free_inodes--;
            get_sb()->free_blocks--;
            return 0;
        }
    }

    free_bit(get_data_bm(), d_idx);
    free_bit(get_inode_bm(), ino);
    return -ENOSPC;
}

static int myfs_rmdir(const char *path) {
    if (strcmp(path, "/") == 0) return -EBUSY;

    char name[MAX_FILENAME_LEN];
    int pino = resolve_parent(path, name);
    if (pino < 0) return pino;

    struct inode *parent = get_inode(pino);
    struct dir_entry *pde = (struct dir_entry *)get_block(parent->blocks[0]);

    for (int i = 0; i < (int)DIR_CAP; i++) {
        if (pde[i].valid == 1 && strcmp(pde[i].name, name) == 0) {
            int dino = pde[i].inode_no;
            struct inode *dir = get_inode(dino);
            if (!(dir->mode & S_IFDIR)) return -ENOTDIR;

            struct dir_entry *de = (struct dir_entry *)get_block(dir->blocks[0]);
            for (int j = 0; j < (int)DIR_CAP; j++) {
                if (de[j].valid == 1 && strcmp(de[j].name, ".") != 0 && strcmp(de[j].name, "..") != 0)
                    return -ENOTEMPTY;
            }

            /* clean up any soft-deleted entries sitting in this dir */
            for (int j = 0; j < (int)DIR_CAP; j++) {
                if (de[j].valid == 2) {
                    purge_inode(de[j].inode_no);
                    de[j].valid = 0;
                }
            }

            purge_inode(dino);
            pde[i].valid = 0;
            return 0;
        }
    }
    return -ENOENT;
}

static int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    if (resolve_path(path) >= 0) return -EEXIST;

    char name[MAX_FILENAME_LEN];
    int pino = resolve_parent(path, name);
    if (pino < 0) return pino;

    struct inode *parent = get_inode(pino);
    if (!(parent->mode & S_IFDIR)) return -ENOTDIR;

    struct dir_entry *de = (struct dir_entry *)get_block(parent->blocks[0]);

    /* if same filename is sitting in trash, wipe it before reusing name */
    for (int i = 0; i < (int)DIR_CAP; i++) {
        if (de[i].valid == 2 && strcmp(de[i].name, name) == 0) {
            purge_inode(de[i].inode_no);
            de[i].valid = 0;
        }
    }

    int ino = alloc_bit(get_inode_bm(), NUM_INODES);
    if (ino < 0) return ino;

    struct inode *in = get_inode(ino);
    memset(in, 0, sizeof(struct inode));
    in->mode = S_IFREG | mode;
    in->size = 0;
    in->atime = in->mtime = in->ctime = time(NULL);

    for (int i = 0; i < (int)DIR_CAP; i++) {
        if (de[i].valid == 0) {
            strncpy(de[i].name, name, MAX_FILENAME_LEN - 1);
            de[i].name[MAX_FILENAME_LEN - 1] = '\0';
            de[i].inode_no = ino;
            de[i].valid = 1;
            get_sb()->free_inodes--;
            return 0;
        }
    }

    free_bit(get_inode_bm(), ino);
    return -ENOSPC;
}

static int myfs_unlink(const char *path) {
    char name[MAX_FILENAME_LEN];
    int pino = resolve_parent(path, name);
    if (pino < 0) return pino;

    struct inode *parent = get_inode(pino);
    struct dir_entry *de = (struct dir_entry *)get_block(parent->blocks[0]);

    for (int i = 0; i < (int)DIR_CAP; i++) {
        if (de[i].valid == 1 && strcmp(de[i].name, name) == 0) {
            if (get_inode(de[i].inode_no)->mode & S_IFDIR)
                return -EISDIR;
            de[i].valid = 2; /* state 2 = soft-deleted */
            return 0;
        }
    }
    return -ENOENT;
}

static int myfs_rename(const char *from, const char *to, unsigned int flags) {
    (void)flags;
    char sname[MAX_FILENAME_LEN], dname[MAX_FILENAME_LEN];
    int sp = resolve_parent(from, sname);
    int dp = resolve_parent(to, dname);
    if (sp < 0) return sp;
    if (dp < 0) return dp;

    struct dir_entry *sde = (struct dir_entry *)get_block(get_inode(sp)->blocks[0]);
    int idx = -1;
    for (int i = 0; i < (int)DIR_CAP; i++) {
        if (sde[i].valid == 1 && strcmp(sde[i].name, sname) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -ENOENT;
    uint32_t target_ino = sde[idx].inode_no;

    if (resolve_path(to) >= 0)
        myfs_unlink(to);

    struct dir_entry *dde = (struct dir_entry *)get_block(get_inode(dp)->blocks[0]);
    for (int i = 0; i < (int)DIR_CAP; i++) {
        if (dde[i].valid == 0) {
            strncpy(dde[i].name, dname, MAX_FILENAME_LEN - 1);
            dde[i].name[MAX_FILENAME_LEN - 1] = '\0';
            dde[i].inode_no = target_ino;
            dde[i].valid = 1;
            sde[idx].valid = 0;
            return 0;
        }
    }
    return -ENOSPC;
}

static int myfs_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    int ino = resolve_path(path);
    if (ino < 0) return ino;

    struct inode *in = get_inode(ino);
    if (size == 0) {
        for (int b = 0; b < DIRECT_BLOCKS; b++) {
            if (in->blocks[b]) {
                free_bit(get_data_bm(), in->blocks[b] - DATA_START_BLOCK);
                in->blocks[b] = 0;
                get_sb()->free_blocks++;
            }
        }
    }
    in->size = size;
    in->mtime = time(NULL);
    return 0;
}

static int myfs_open(const char *path, struct fuse_file_info *fi) {
    if (is_virtual_file(path)) return 0;

    int ino = resolve_path(path);
    if (ino < 0) return ino;

    if (fi->flags & O_TRUNC)
        return myfs_truncate(path, 0, fi);

    return 0;
}

static int handle_virtual_read(const char *path, char *buf, size_t size, off_t offset) {
    if (strcmp(path, "/.recover") == 0) {
        if (offset == 0) {
            int n = 0;
            for (int d = 0; d < NUM_INODES; d++) {
                if (test_bit(get_inode_bm(), d) && (get_inode(d)->mode & S_IFDIR)) {
                    struct dir_entry *de = (struct dir_entry *)get_block(get_inode(d)->blocks[0]);
                    for (int i = 0; i < (int)DIR_CAP; i++) {
                        if (de[i].valid == 2) {
                            de[i].valid = 1;
                            n++;
                        }
                    }
                }
            }
            snprintf(ctl_buf, sizeof(ctl_buf),
                     "[MYFS RECOVERY] Restored %d deleted file(s) from Trash!\n", n);
        }
        size_t len = strlen(ctl_buf);
        if ((size_t)offset >= len) return 0;
        if (size > len - offset) size = len - offset;
        memcpy(buf, ctl_buf + offset, size);
        return size;
    }

    if (strcmp(path, "/.purge") == 0) {
        if (offset == 0) {
            int n = 0;
            for (int d = 0; d < NUM_INODES; d++) {
                if (test_bit(get_inode_bm(), d) && (get_inode(d)->mode & S_IFDIR)) {
                    struct dir_entry *de = (struct dir_entry *)get_block(get_inode(d)->blocks[0]);
                    for (int i = 0; i < (int)DIR_CAP; i++) {
                        if (de[i].valid == 2) {
                            purge_inode(de[i].inode_no);
                            de[i].valid = 0;
                            n++;
                        }
                    }
                }
            }
            snprintf(ctl_buf, sizeof(ctl_buf),
                     "[MYFS PURGE] Permanently freed blocks & inodes for %d Trash file(s).\n", n);
        }
        size_t len = strlen(ctl_buf);
        if ((size_t)offset >= len) return 0;
        if (size > len - offset) size = len - offset;
        memcpy(buf, ctl_buf + offset, size);
        return size;
    }

    /* /.diskstat */
    char out[1024];
    char bmap[65];
    uint8_t *bm = get_data_bm();
    for (int i = 0; i < 64; i++)
        bmap[i] = test_bit(bm, i) ? '#' : '.';
    bmap[64] = '\0';

    int trashed = 0;
    for (int d = 0; d < NUM_INODES; d++) {
        if (test_bit(get_inode_bm(), d) && (get_inode(d)->mode & S_IFDIR)) {
            struct dir_entry *de = (struct dir_entry *)get_block(get_inode(d)->blocks[0]);
            for (int i = 0; i < (int)DIR_CAP; i++) {
                if (de[i].valid == 2) trashed++;
            }
        }
    }

    uint32_t u_blk = DATA_CAP - get_sb()->free_blocks;
    uint32_t u_ino = get_sb()->total_inodes - get_sb()->free_inodes;

    int len = snprintf(out, sizeof(out),
        "========================================\n"
        "       MYFS LIVE STORAGE MONITOR        \n"
        "========================================\n"
        "Magic Number : 0x%X\n"
        "Block Size   : %u Bytes\n"
        "Data Blocks  : %u Used / %u Free (Total: %u)\n"
        "Inodes       : %u Used / %u Free (Total: %u)\n"
        "Trash Bin    : %d file(s) awaiting recovery/purge\n"
        "----------------------------------------\n"
        "Live Data Block Map (First 64 Blocks):\n"
        "[%.32s]\n"
        "[%.32s]\n"
        "Legend: '#' = Allocated | '.' = Free\n"
        "Commands: cat .recover | cat .purge\n"
        "========================================\n",
        get_sb()->magic, get_sb()->block_size,
        u_blk, get_sb()->free_blocks, get_sb()->total_blocks,
        u_ino, get_sb()->free_inodes, get_sb()->total_inodes,
        trashed, bmap, bmap + 32);

    if (offset >= len) return 0;
    if (size > (size_t)(len - offset)) size = len - offset;
    memcpy(buf, out + offset, size);
    return size;
}

static int myfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    if (is_virtual_file(path))
        return handle_virtual_read(path, buf, size, offset);

    int ino = resolve_path(path);
    if (ino < 0) return ino;

    struct inode *in = get_inode(ino);
    if (offset >= in->size) return 0;
    if (offset + size > in->size)
        size = in->size - offset;

    size_t done = 0;
    while (done < size) {
        off_t pos = offset + done;
        int b_idx = pos / BLOCK_SIZE;
        size_t b_off = pos % BLOCK_SIZE;
        size_t chunk = BLOCK_SIZE - b_off;
        if (chunk > size - done)
            chunk = size - done;

        if (in->blocks[b_idx] == 0) {
            memset(buf + done, 0, chunk);
        } else {
            char *blk = (char *)get_block(in->blocks[b_idx]);
            memcpy(buf + done, blk + b_off, chunk);
        }
        done += chunk;
    }
    return done;
}

static int myfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    int ino = resolve_path(path);
    if (ino < 0) return ino;

    struct inode *in = get_inode(ino);
    size_t max_sz = DIRECT_BLOCKS * BLOCK_SIZE;

    if ((size_t)offset >= max_sz) return -EFBIG;
    if (offset + size > max_sz)
        size = max_sz - offset;

    size_t done = 0;
    while (done < size) {
        off_t pos = offset + done;
        int b_idx = pos / BLOCK_SIZE;
        size_t b_off = pos % BLOCK_SIZE;
        size_t chunk = BLOCK_SIZE - b_off;
        if (chunk > size - done)
            chunk = size - done;

        if (in->blocks[b_idx] == 0) {
            int d_idx = alloc_bit(get_data_bm(), DATA_CAP);
            if (d_idx < 0) {
                if (done > 0) break;
                return -ENOSPC;
            }
            in->blocks[b_idx] = DATA_START_BLOCK + d_idx;
            get_sb()->free_blocks--;
        }

        char *blk = (char *)get_block(in->blocks[b_idx]);
        memcpy(blk + b_off, buf + done, chunk);
        done += chunk;
    }

    if (offset + done > in->size)
        in->size = offset + done;

    in->mtime = time(NULL);
    return done;
}

static int myfs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    (void)fi;
    int ino = resolve_path(path);
    if (ino < 0) return ino;

    struct inode *in = get_inode(ino);
    in->atime = tv[0].tv_sec;
    in->mtime = tv[1].tv_sec;
    return 0;
}

static int myfs_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    memset(stbuf, 0, sizeof(struct statvfs));
    stbuf->f_bsize   = BLOCK_SIZE;
    stbuf->f_frsize  = BLOCK_SIZE;
    stbuf->f_blocks  = DATA_CAP;
    stbuf->f_bfree   = get_sb()->free_blocks;
    stbuf->f_bavail  = get_sb()->free_blocks;
    stbuf->f_files   = NUM_INODES;
    stbuf->f_ffree   = get_sb()->free_inodes;
    stbuf->f_favail  = get_sb()->free_inodes;
    stbuf->f_namemax = MAX_FILENAME_LEN - 1;
    return 0;
}

static struct fuse_operations my_ops = {
    .init     = myfs_init,
    .getattr  = myfs_getattr,
    .readdir  = myfs_readdir,
    .mkdir    = myfs_mkdir,
    .rmdir    = myfs_rmdir,
    .open     = myfs_open,
    .create   = myfs_create,
    .read     = myfs_read,
    .write    = myfs_write,
    .unlink   = myfs_unlink,
    .rename   = myfs_rename,
    .truncate = myfs_truncate,
    .utimens  = myfs_utimens,
    .statfs   = myfs_statfs,
};

int main(int argc, char *argv[]) {
    /* fuse_main chdirs to "/", so cache absolute path to disk.img first */
    if (!realpath("disk.img", disk_path)) {
        perror("disk.img not found (run ./mkfs_myfs first)");
        return 1;
    }
    return fuse_main(argc, argv, &my_ops, NULL);
}