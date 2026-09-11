#ifndef LFS_H
#define LFS_H

#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Constants ─────────────────────────────────────────────────────────── */

#define LOGFS_MAGIC         0x4C4F4746U
#define LOGFS_VERSION       1U
#define LOGFS_BLOCK_SIZE    4096U
#define LOGFS_SEG_BLOCKS    64U
#define LOGFS_MAX_INODES    1024U
#define LOGFS_MAX_SEGMENTS  1024U
#define LOGFS_IMAGE_SIZE    (256U * 1024U * 1024U)
#define LOGFS_TOTAL_BLOCKS  (LOGFS_IMAGE_SIZE / LOGFS_BLOCK_SIZE)
#define LOGFS_NDIRECT       12U
#define LOGFS_MAX_FILENAME  255U

#define SEG_MAGIC           0x5345474DU
#define REC_MAGIC           0x52454352U

#define BLK_FREE           (-1)
#define BLK_INODE          (-2)

/* checkpoint occupies blocks 1..CPBlocks-1 */
#define CP_IMAP_ENTRIES     LOGFS_MAX_INODES
#define CP_INODE_BMAP_BYTES LOGFS_MAX_INODES
#define CP_SEG_BMAP_BYTES   LOGFS_MAX_SEGMENTS

#define CHECKPOINT_RAW_SIZE (CP_IMAP_ENTRIES * sizeof(uint64_t) + \
                             CP_INODE_BMAP_BYTES +              \
                             CP_SEG_BMAP_BYTES +                \
                             sizeof(uint32_t))
#define CHECKPOINT_BLOCKS   ((CHECKPOINT_RAW_SIZE + LOGFS_BLOCK_SIZE - 1) / LOGFS_BLOCK_SIZE)

#define LOG_START_BLOCK     (1U + CHECKPOINT_BLOCKS)

/* ── On-disk structures (all little-endian, packed) ────────────────────── */

enum logfs_op {
    OP_CREATE = 1,
    OP_MKDIR  = 2,
    OP_WRITE  = 3,
    OP_RMDIR  = 4,
};

struct logfs_super {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t segment_blocks;
    uint32_t max_inodes;
    uint32_t num_segments;
    uint64_t total_blocks;
    uint64_t checkpoint_block;
    uint64_t log_start_block;
    uint32_t crc32;
    char     _pad[LOGFS_BLOCK_SIZE - 56];
} __attribute__((packed));

struct logfs_checkpoint {
    uint64_t imap[CP_IMAP_ENTRIES];
    uint8_t  inode_bitmap[CP_INODE_BMAP_BYTES];
    uint8_t  segment_bitmap[CP_SEG_BMAP_BYTES];
    uint32_t crc32;
    char     _pad[CHECKPOINT_BLOCKS * LOGFS_BLOCK_SIZE - CHECKPOINT_RAW_SIZE];
} __attribute__((packed));

struct logfs_segment_hdr {
    uint32_t magic;
    uint32_t seg_seq;
    uint32_t flags;
    int32_t  block_map[LOGFS_SEG_BLOCKS];
    uint32_t crc32;
    char     _pad[LOGFS_BLOCK_SIZE - (4 + 4 + 4 +
              LOGFS_SEG_BLOCKS * (int32_t)sizeof(int32_t) +
              sizeof(uint32_t))];
} __attribute__((packed));

struct logfs_inode {
    uint64_t ino;
    uint32_t mode;
    uint64_t size;
    int64_t  mtime;
    uint64_t direct[LOGFS_NDIRECT];
} __attribute__((packed));

struct logfs_dirent {
    uint64_t ino;
    uint16_t name_len;
    char     name[LOGFS_MAX_FILENAME];
} __attribute__((packed));

struct logfs_rec_hdr {
    uint32_t magic;
    uint32_t op;
    uint32_t nblocks;
    uint32_t crc32;
} __attribute__((packed));

/* ── In-memory caches ──────────────────────────────────────────────────── */

#define DATA_CACHE_SIZE  64
#define INODE_CACHE_SIZE 64

typedef struct {
    uint64_t block_no;
    uint8_t  data[LOGFS_BLOCK_SIZE];
    bool     valid;
    bool     dirty;
} data_cache_entry_t;

typedef struct {
    uint64_t       ino;
    struct logfs_inode inode;
    bool           valid;
    bool           dirty;
} inode_cache_entry_t;

typedef struct {
    data_cache_entry_t  entries[DATA_CACHE_SIZE];
    int                 lru[DATA_CACHE_SIZE]; /* most-recently-used first */
} data_cache_t;

typedef struct {
    inode_cache_entry_t entries[INODE_CACHE_SIZE];
    int                 lru[INODE_CACHE_SIZE];
} inode_cache_t;

/* ── Segment write buffer ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  buf[LOGFS_SEG_BLOCKS * LOGFS_BLOCK_SIZE];
    uint32_t cur_seg;          /* segment number being written to            */
    uint32_t seg_seq;          /* sequence number for next segment header    */
    int      next_slot;        /* next free block index in segment (1-based) */
    bool     active;          /* true once we have a live open segment       */
} seg_write_buf_t;

/* ── Per-move record (used by lfs_gc when forwarding live blocks) ──────── */

typedef struct {
    uint32_t blk_offset;  /* block offset within the segment write buffer */
    uint32_t nblocks;     /* 1 = data or inode block                       */
    enum logfs_op op;     /* the GC synthesises OP_WRITE records          */
} gc_record_t;

/* ── Global LFS context ────────────────────────────────────────────────── */

typedef struct {
    int                 fd;
    char                img_path[256];

    struct logfs_super       super;
    struct logfs_checkpoint  cp;

    data_cache_t             data_cache;
    inode_cache_t            inode_cache;

    seg_write_buf_t          wb;   /* segment write buffer */

    bool                     mounted;
    uint32_t                 next_seg_seq;
} lfs_ctx_t;

/* ── Library API (Phase 1 + Phase 2 thin wrappers) ────────────────────── */

int  lfs_mount(const char *img_path);
int  lfs_umount(void);

int  lfs_create(const char *path, mode_t mode);
int  lfs_mkdir(const char *path, mode_t mode);
int  lfs_write(const char *path, const char *buf, size_t size, off_t offset);
int  lfs_rmdir(const char *path);

int  lfs_lookup(const char *path, uint64_t *ino_out);
int  lfs_getattr(const char *path, struct stat *stbuf);

typedef int (*lfs_dir_filler_t)(void *buf, const char *name,
                                uint64_t ino, mode_t mode);
int  lfs_readdir(const char *path, void *buf, lfs_dir_filler_t filler);
int  lfs_read(const char *path, char *buf, size_t size, off_t offset);

int  lfs_gc(void);

/* ── Utility ───────────────────────────────────────────────────────────── */

uint32_t lfs_crc32(const void *data, size_t len);

#endif /* LFS_H */
