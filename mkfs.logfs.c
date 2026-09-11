/* mkfs.logfs.c  –  Format a 256 MiB logfs.img (Task A.8) */

#define _GNU_SOURCE
#include "lfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    const char *path = "logfs.img";
    if (argc > 1) path = argv[1];

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(path); return 1; }

    /* preallocate 256 MiB */
    if (ftruncate(fd, LOGFS_IMAGE_SIZE) < 0) {
        perror("ftruncate"); close(fd); return 1;
    }

    /* ── superblock (block 0) ─────────────────────────────────────────── */
    struct logfs_super sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic          = LOGFS_MAGIC;
    sb.version        = LOGFS_VERSION;
    sb.block_size     = LOGFS_BLOCK_SIZE;
    sb.segment_blocks = LOGFS_SEG_BLOCKS;
    sb.max_inodes     = LOGFS_MAX_INODES;
    sb.num_segments   = (LOGFS_IMAGE_SIZE / LOGFS_BLOCK_SIZE -
                          LOG_START_BLOCK) / LOGFS_SEG_BLOCKS;
    sb.total_blocks   = LOGFS_TOTAL_BLOCKS;
    sb.checkpoint_block = 1;
    sb.log_start_block  = LOG_START_BLOCK;
    sb.crc32 = 0;
    sb.crc32 = lfs_crc32(&sb,
                          sizeof(struct logfs_super) - sizeof(uint32_t));

    uint8_t blk0[LOGFS_BLOCK_SIZE];
    memset(blk0, 0, sizeof(blk0));
    memcpy(blk0, &sb, sizeof(sb));
    pwrite(fd, blk0, LOGFS_BLOCK_SIZE, 0);

    /* ── checkpoint (blocks 1..) ──────────────────────────────────────── */
    struct logfs_checkpoint cp;
    memset(&cp, 0, sizeof(cp));

    /* root inode = 0, mark allocated */
    cp.inode_bitmap[0 / 8] |= (1U << (0 % 8));

    /* segment 0 in use */
    cp.segment_bitmap[0] = 1;

    cp.crc32 = 0;
    cp.crc32 = lfs_crc32(&cp, CHECKPOINT_RAW_SIZE);

    uint8_t cp_buf[CHECKPOINT_BLOCKS * LOGFS_BLOCK_SIZE];
    memset(cp_buf, 0, sizeof(cp_buf));
    memcpy(cp_buf, &cp, CHECKPOINT_RAW_SIZE);
    for (uint32_t i = 0; i < CHECKPOINT_BLOCKS; i++)
        pwrite(fd, cp_buf + i * LOGFS_BLOCK_SIZE,
               LOGFS_BLOCK_SIZE, (1 + i) * LOGFS_BLOCK_SIZE);

    /* ── root inode (in segment 0, block LOG_START_BLOCK) ────────────── */
    struct logfs_inode root;
    memset(&root, 0, sizeof(root));
    root.ino  = 0;
    root.mode = S_IFDIR | 0755;
    root.size = LOGFS_BLOCK_SIZE; /* one empty data block */
    root.mtime = time(NULL);
    for (int i = 0; i < LOGFS_NDIRECT; i++) root.direct[i] = 0;

    /* The root inode will be the first inode block in segment 0.
       The first record in the log is a OP_MKDIR-like record that
       creates the root directory.  We build it manually here. */

    uint64_t root_inode_blk = LOG_START_BLOCK + 1; /* block after seg header */

    /* empty directory data block (block 2 = LOG_START_BLOCK + 2) */
    uint8_t dir_data[LOGFS_BLOCK_SIZE];
    memset(dir_data, 0, LOGFS_BLOCK_SIZE);
    struct logfs_dirent sentinel;
    memset(&sentinel, 0, sizeof(sentinel));
    memcpy(dir_data, &sentinel, sizeof(sentinel));

    /* write root inode block */
    uint8_t inode_blk[LOGFS_BLOCK_SIZE];
    memset(inode_blk, 0, sizeof(inode_blk));
    memcpy(inode_blk, &root, sizeof(root));
    pwrite(fd, inode_blk, LOGFS_BLOCK_SIZE,
           root_inode_blk * LOGFS_BLOCK_SIZE);

    /* write empty dir data block */
    pwrite(fd, dir_data, LOGFS_BLOCK_SIZE,
           (LOG_START_BLOCK + 2) * LOGFS_BLOCK_SIZE);

    /* update imap[0] → root inode block */
    cp.imap[0] = root_inode_blk;
    cp.crc32 = 0;
    cp.crc32 = lfs_crc32(&cp, CHECKPOINT_RAW_SIZE);
    memset(cp_buf, 0, sizeof(cp_buf));
    memcpy(cp_buf, &cp, CHECKPOINT_RAW_SIZE);
    for (uint32_t i = 0; i < CHECKPOINT_BLOCKS; i++)
        pwrite(fd, cp_buf + i * LOGFS_BLOCK_SIZE,
               LOGFS_BLOCK_SIZE, (1 + i) * LOGFS_BLOCK_SIZE);

    /* ── segment 0 header ─────────────────────────────────────────────── */
    struct logfs_segment_hdr shdr;
    memset(&shdr, 0, sizeof(shdr));
    shdr.magic   = SEG_MAGIC;
    shdr.seg_seq = 0;
    shdr.flags   = 1; /* sealed */
    for (int i = 0; i < LOGFS_SEG_BLOCKS; i++)
        shdr.block_map[i] = BLK_FREE;
    shdr.block_map[0] = BLK_INODE; /* header itself */
    shdr.block_map[1] = BLK_INODE; /* root inode */
    shdr.block_map[2] = 0;         /* root dir data (inode 0) */
    set_crc(&shdr, sizeof(shdr) - sizeof(uint32_t));
    uint8_t shdr_blk[LOGFS_BLOCK_SIZE];
    memset(shdr_blk, 0, sizeof(shdr_blk));
    memcpy(shdr_blk, &shdr, sizeof(shdr));
    pwrite(fd, shdr_blk, LOGFS_BLOCK_SIZE,
           LOG_START_BLOCK * LOGFS_BLOCK_SIZE);

    /* ── initial log record: OP_MKDIR for root ────────────────────────── */
    struct logfs_rec_hdr rhdr;
    memset(&rhdr, 0, sizeof(rhdr));
    rhdr.magic   = REC_MAGIC;
    rhdr.op      = OP_MKDIR;
    rhdr.nblocks = 3; /* root inode + dir data + parent inode(=root) */
    set_crc(&rhdr, sizeof(rhdr));

    uint64_t rec_blk = LOG_START_BLOCK + 3; /* block after seg hdr + inode + data */
    uint8_t rec_buf[LOGFS_BLOCK_SIZE];
    memset(rec_buf, 0, sizeof(rec_buf));
    memcpy(rec_buf, &rhdr, sizeof(rhdr));
    pwrite(fd, rec_buf, LOGFS_BLOCK_SIZE,
           rec_blk * LOGFS_BLOCK_SIZE);

    /* payload blocks: root inode, dir data, parent (root) inode */
    pwrite(fd, inode_blk, LOGFS_BLOCK_SIZE,
           (rec_blk + 1) * LOGFS_BLOCK_SIZE);
    pwrite(fd, dir_data, LOGFS_BLOCK_SIZE,
           (rec_blk + 2) * LOGFS_BLOCK_SIZE);
    pwrite(fd, inode_blk, LOGFS_BLOCK_SIZE,
           (rec_blk + 3) * LOGFS_BLOCK_SIZE);

    /* update segment header block_map for the record slots */
    shdr.block_map[3] = BLK_INODE; /* record header */
    shdr.block_map[4] = BLK_INODE; /* root inode payload */
    shdr.block_map[5] = 0;         /* dir data payload */
    shdr.block_map[6] = BLK_INODE; /* parent inode payload */
    set_crc(&shdr, sizeof(shdr) - sizeof(uint32_t));
    memcpy(shdr_blk, &shdr, sizeof(shdr));
    pwrite(fd, shdr_blk, LOGFS_BLOCK_SIZE,
           LOG_START_BLOCK * LOGFS_BLOCK_SIZE);

    fsync(fd);
    close(fd);
    printf("mkfs.logfs: created %s (%u MiB, %u blocks/seg)\n",
           path, LOGFS_IMAGE_SIZE / (1024 * 1024), LOGFS_SEG_BLOCKS);
    return 0;
}
