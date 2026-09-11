/* lfs.c  –  Log-structured filesystem library (Tasks A–D) */

#define _GNU_SOURCE
#include "lfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
   Global context
   ═══════════════════════════════════════════════════════════════════════════ */

static lfs_ctx_t C;

/* forward declarations */
static int  wb_flush(void);
static int  wb_init(uint32_t seg);
static int  wb_append_record(uint32_t op, const uint8_t *payload,
                             uint32_t nblocks, const int32_t *bmap);
static void cp_write_back(void);
static void super_write_back(void);
static int  read_block(uint64_t blk, void *buf);
static int  read_block_cached(uint64_t blk, void *buf);
static void data_cache_invalidate(uint64_t blk);
static void inode_cache_invalidate(uint64_t ino);
static int  alloc_inode(void);
static int  alloc_seg(void);
static int  path_resolve(const char *path, uint64_t *ino_out);

/* ═══════════════════════════════════════════════════════════════════════════
   CRC-32 (Castagnoli, matches common implementations)
   ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t crc_table[256];
static int      crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

uint32_t lfs_crc32(const void *data, size_t len)
{
    if (!crc_ready) crc_init();
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

static void set_crc(void *hdr, size_t hdr_size)
{
    struct logfs_rec_hdr *r = hdr;
    r->crc32 = 0;
    r->crc32 = lfs_crc32(hdr, hdr_size);
}

static int verify_crc(const void *hdr, size_t hdr_size)
{
    struct logfs_rec_hdr *r = (void *)hdr;
    uint32_t saved = r->crc32;
    r->crc32 = 0;
    uint32_t got = lfs_crc32(hdr, hdr_size);
    r->crc32 = saved;
    return (got == saved) ? 0 : -EIO;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Low-level disk I/O
   ═══════════════════════════════════════════════════════════════════════════ */

static int read_block(uint64_t blk, void *buf)
{
    off_t off = (off_t)blk * LOGFS_BLOCK_SIZE;
    ssize_t n = pread(C.fd, buf, LOGFS_BLOCK_SIZE, off);
    return (n == LOGFS_BLOCK_SIZE) ? 0 : -EIO;
}

static int write_block(uint64_t blk, const void *buf)
{
    off_t off = (off_t)blk * LOGFS_BLOCK_SIZE;
    ssize_t n = pwrite(C.fd, buf, LOGFS_BLOCK_SIZE, off);
    return (n == LOGFS_BLOCK_SIZE) ? 0 : -EIO;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Data block cache  (LRU, 64 entries)
   ═══════════════════════════════════════════════════════════════════════════ */

static void dc_init(void)
{
    memset(&C.data_cache, 0, sizeof(C.data_cache));
    for (int i = 0; i < DATA_CACHE_SIZE; i++)
        C.data_cache.lru[i] = i;
}

/* return index or -1 */
static int dc_find(uint64_t blk)
{
    for (int i = 0; i < DATA_CACHE_SIZE; i++)
        if (C.data_cache.entries[i].valid &&
            C.data_cache.entries[i].block_no == blk)
            return i;
    return -1;
}

static void dc_touch(int idx)
{
    int val = C.data_cache.lru[idx];
    for (int i = idx; i > 0; i--)
        C.data_cache.lru[i] = C.data_cache.lru[i - 1];
    C.data_cache.lru[0] = val;
}

static int dc_evict(void)
{
    for (int i = DATA_CACHE_SIZE - 1; i >= 0; i--) {
        int idx = C.data_cache.lru[i];
        if (C.data_cache.entries[idx].dirty) {
            off_t off = (off_t)C.data_cache.entries[idx].block_no *
                        LOGFS_BLOCK_SIZE;
            if (pwrite(C.fd, C.data_cache.entries[idx].data,
                       LOGFS_BLOCK_SIZE, off) != LOGFS_BLOCK_SIZE)
                return -EIO;
        }
        C.data_cache.entries[idx].valid = 0;
        C.data_cache.entries[idx].dirty = 0;
        return idx;
    }
    return -1;
}

static int dc_insert(uint64_t blk, const void *data)
{
    int idx = dc_find(blk);
    if (idx >= 0) {
        memcpy(C.data_cache.entries[idx].data, data, LOGFS_BLOCK_SIZE);
        C.data_cache.entries[idx].dirty = 1;
        dc_touch(idx);
        return idx;
    }
    idx = dc_evict();
    if (idx < 0) return -ENOSPC;
    C.data_cache.entries[idx].block_no = blk;
    memcpy(C.data_cache.entries[idx].data, data, LOGFS_BLOCK_SIZE);
    C.data_cache.entries[idx].valid  = 1;
    C.data_cache.entries[idx].dirty  = 1;
    C.data_cache.lru[0] = idx;
    return idx;
}

static void data_cache_invalidate(uint64_t blk)
{
    int idx = dc_find(blk);
    if (idx >= 0) {
        C.data_cache.entries[idx].valid = 0;
        C.data_cache.entries[idx].dirty = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Inode cache  (LRU, 64 entries)
   ═══════════════════════════════════════════════════════════════════════════ */

static void ic_init(void)
{
    memset(&C.inode_cache, 0, sizeof(C.inode_cache));
    for (int i = 0; i < INODE_CACHE_SIZE; i++)
        C.inode_cache.lru[i] = i;
}

static int ic_find(uint64_t ino)
{
    for (int i = 0; i < INODE_CACHE_SIZE; i++)
        if (C.inode_cache.entries[i].valid &&
            C.inode_cache.entries[i].ino == ino)
            return i;
    return -1;
}

static void ic_touch(int idx)
{
    int val = C.inode_cache.lru[idx];
    for (int i = idx; i > 0; i--)
        C.inode_cache.lru[i] = C.inode_cache.lru[i - 1];
    C.inode_cache.lru[0] = val;
}

static int ic_evict(void)
{
    for (int i = INODE_CACHE_SIZE - 1; i >= 0; i--) {
        int idx = C.inode_cache.lru[i];
        C.inode_cache.entries[idx].valid = 0;
        C.inode_cache.entries[idx].dirty = 0;
        return idx;
    }
    return -1;
}

static int ic_insert(uint64_t ino, const struct logfs_inode *ip)
{
    int idx = ic_find(ino);
    if (idx >= 0) {
        memcpy(&C.inode_cache.entries[idx].inode, ip,
               sizeof(struct logfs_inode));
        C.inode_cache.entries[idx].dirty = 1;
        ic_touch(idx);
        return idx;
    }
    idx = ic_evict();
    if (idx < 0) return -ENOSPC;
    C.inode_cache.entries[idx].ino = ino;
    memcpy(&C.inode_cache.entries[idx].inode, ip,
           sizeof(struct logfs_inode));
    C.inode_cache.entries[idx].valid = 1;
    C.inode_cache.entries[idx].dirty = 1;
    C.inode_cache.lru[0] = idx;
    return idx;
}

static void inode_cache_invalidate(uint64_t ino)
{
    int idx = ic_find(ino);
    if (idx >= 0) {
        C.inode_cache.entries[idx].valid = 0;
        C.inode_cache.entries[idx].dirty = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Read-through: block number → data (checks caches, write buffer, disk)
   ═══════════════════════════════════════════════════════════════════════════ */

static int read_block_cached(uint64_t blk, void *buf)
{
    if (blk == 0) { memset(buf, 0, LOGFS_BLOCK_SIZE); return 0; }

    /* 1. data cache */
    int idx = dc_find(blk);
    if (idx >= 0) {
        memcpy(buf, C.data_cache.entries[idx].data, LOGFS_BLOCK_SIZE);
        dc_touch(idx);
        return 0;
    }

    /* 2. open write buffer (block may be unflushed) */
    if (C.wb.active) {
        uint64_t seg_start = LOG_START_BLOCK +
                             (uint64_t)C.wb.cur_seg * LOGFS_SEG_BLOCKS;
        if (blk >= seg_start &&
            blk < seg_start + (uint64_t)C.wb.next_slot) {
            uint32_t off_in_seg = (uint32_t)(blk - seg_start);
            memcpy(buf, C.wb.buf + off_in_seg * LOGFS_BLOCK_SIZE,
                   LOGFS_BLOCK_SIZE);
            return 0;
        }
    }

    /* 3. disk */
    int r = read_block(blk, buf);
    if (r) return r;
    dc_insert(blk, buf);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Inode I/O (via imap → data-block cache)
   ═══════════════════════════════════════════════════════════════════════════ */

static uint64_t imap_get(uint64_t ino)
{
    if (ino >= LOGFS_MAX_INODES) return 0;
    return C.cp.imap[ino];
}

static void imap_set(uint64_t ino, uint64_t blk)
{
    if (ino < LOGFS_MAX_INODES)
        C.cp.imap[ino] = blk;
}

static int load_inode(uint64_t ino, struct logfs_inode *out)
{
    /* cache hit? */
    int idx = ic_find(ino);
    if (idx >= 0) {
        memcpy(out, &C.inode_cache.entries[idx].inode,
               sizeof(struct logfs_inode));
        ic_touch(idx);
        return 0;
    }
    uint64_t blk = imap_get(ino);
    if (blk == 0) return -ENOENT;
    uint8_t tmp[LOGFS_BLOCK_SIZE];
    int r = read_block_cached(blk, tmp);
    if (r) return r;
    memcpy(out, tmp, sizeof(struct logfs_inode));
    ic_insert(ino, out);
    return 0;
}

static void save_inode(const struct logfs_inode *ip)
{
    uint64_t blk = imap_get(ip->ino);
    /* invalidate old cache entry so stale copy is not used */
    data_cache_invalidate(blk);
    dc_insert(blk, ip);
    ic_insert(ip->ino, ip);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Segment write buffer
   ═══════════════════════════════════════════════════════════════════════════ */

/* Allocate a fresh free segment (returns seg number, marks bitmap). */
static int alloc_seg(void)
{
    for (uint32_t s = 0; s < LOGFS_MAX_SEGMENTS; s++) {
        if (!C.cp.segment_bitmap[s]) {
            C.cp.segment_bitmap[s] = 1;
            return (int)s;
        }
    }
    return -ENOSPC;
}

/* Write out the current write-buffer segment and prepare a new one. */
static int wb_flush(void)
{
    if (!C.wb.active || C.wb.next_slot == 0) return 0;

    uint64_t seg_start = LOG_START_BLOCK +
                         (uint64_t)C.wb.cur_seg * LOGFS_SEG_BLOCKS;

    /* build and write the segment header */
    struct logfs_segment_hdr shdr;
    memset(&shdr, 0, sizeof(shdr));
    shdr.magic  = SEG_MAGIC;
    shdr.seg_seq = C.wb.seg_seq;
    shdr.flags  = 1; /* sealed */
    /* copy block_map from first block of the write buffer (where we placed
       the header) */
    memcpy(shdr.block_map, C.wb.buf + offsetof(struct logfs_segment_hdr,
           block_map), sizeof(shdr.block_map));
    set_crc(&shdr, sizeof(shdr) - sizeof(uint32_t));
    memcpy(C.wb.buf, &shdr, sizeof(shdr));

    /* write all 64 blocks */
    for (uint32_t i = 0; i < LOGFS_SEG_BLOCKS; i++) {
        int r = write_block(seg_start + i,
                            C.wb.buf + i * LOGFS_BLOCK_SIZE);
        if (r) return r;
    }
    return 0;
}

/* Prepare a new open segment in the write buffer. */
static int wb_init(uint32_t seg)
{
    memset(C.wb.buf, 0, sizeof(C.wb.buf));
    C.wb.cur_seg   = seg;
    C.wb.seg_seq   = C.next_seg_seq++;
    C.wb.next_slot = 0;
    C.wb.active    = 1;

    /* install a blank segment header at slot 0 */
    struct logfs_segment_hdr shdr;
    memset(&shdr, 0, sizeof(shdr));
    shdr.magic   = SEG_MAGIC;
    shdr.seg_seq = C.wb.seg_seq;
    shdr.flags   = 0; /* open */
    for (int i = 0; i < LOGFS_SEG_BLOCKS; i++)
        shdr.block_map[i] = BLK_FREE;
    set_crc(&shdr, sizeof(shdr) - sizeof(uint32_t));
    memcpy(C.wb.buf, &shdr, sizeof(shdr));
    C.wb.next_slot = 1; /* slot 0 = header */
    return 0;
}

static int wb_ensure_open(void)
{
    if (C.wb.active && C.wb.next_slot > 0 &&
        C.wb.next_slot < LOGFS_SEG_BLOCKS)
        return 0;
    int s = alloc_seg();
    if (s < 0) return s;
    return wb_init((uint32_t)s);
}

/*
 * Append a log record to the current segment write buffer.
 * nblocks = number of payload data blocks after the record header.
 * bmap[0..nblocks-1]: BLK_INODE or >=0 (inode owning data block).
 * Returns 0 on success or -ENOSPC if the segment cannot hold it.
 */
static int wb_append_record(uint32_t op, const uint8_t *payload,
                            uint32_t nblocks, const int32_t *bmap)
{
    uint32_t total = 1 + nblocks; /* header block + payload blocks */

    if (C.wb.next_slot + (int)total > LOGFS_SEG_BLOCKS) {
        int r = wb_flush();
        if (r) return r;
        /* allocate a new segment if needed */
        if (!C.wb.active) {
            int s = alloc_seg();
            if (s < 0) return s;
            wb_init((uint32_t)s);
        }
    }

    uint32_t slot = C.wb.next_slot;

    /* place the record header */
    struct logfs_rec_hdr rhdr;
    memset(&rhdr, 0, sizeof(rhdr));
    rhdr.magic   = REC_MAGIC;
    rhdr.op      = op;
    rhdr.nblocks = nblocks;
    set_crc(&rhdr, sizeof(rhdr));
    memcpy(C.wb.buf + slot * LOGFS_BLOCK_SIZE, &rhdr, sizeof(rhdr));

    /* update the segment header's block_map for this header slot */
    struct logfs_segment_hdr *shdr =
        (struct logfs_segment_hdr *)C.wb.buf;
    shdr->block_map[slot] = BLK_INODE;

    /* place payload blocks */
    uint32_t payload_off = 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t ps = slot + 1 + i;
        memcpy(C.wb.buf + ps * LOGFS_BLOCK_SIZE,
               payload + payload_off * LOGFS_BLOCK_SIZE,
               LOGFS_BLOCK_SIZE);
        shdr->block_map[ps] = bmap[i];
        payload_off++;
    }

    /* update imap for any inode blocks in this record */
    for (uint32_t i = 0; i < nblocks; i++) {
        if (bmap[i] == BLK_INODE) {
            const struct logfs_inode *ip =
                (const struct logfs_inode *)(payload +
                    i * LOGFS_BLOCK_SIZE);
            imap_set(ip->ino, LOG_START_BLOCK +
                     (uint64_t)C.wb.cur_seg * LOGFS_SEG_BLOCKS +
                     slot + 1 + i);
        }
    }

    C.wb.next_slot += total;
    return 0;
}

/* fix imap for inodes appended in wb_append_record (called after append) */
static void wb_update_imap_for_record(uint32_t slot, uint32_t nblocks,
                                      const int32_t *bmap,
                                      const uint8_t *payload)
{
    for (uint32_t i = 0; i < nblocks; i++) {
        if (bmap[i] == BLK_INODE) {
            const struct logfs_inode *ip =
                (const struct logfs_inode *)(payload +
                    i * LOGFS_BLOCK_SIZE);
            uint64_t blk = LOG_START_BLOCK +
                           (uint64_t)C.wb.cur_seg * LOGFS_SEG_BLOCKS +
                           slot + 1 + i;
            imap_set(ip->ino, blk);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Checkpoint / superblock persistence
   ═══════════════════════════════════════════════════════════════════════════ */

static void cp_write_back(void)
{
    C.cp.crc32 = 0;
    C.cp.crc32 = lfs_crc32(&C.cp, CHECKPOINT_RAW_SIZE);
    uint8_t buf[CHECKPOINT_BLOCKS * LOGFS_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &C.cp, CHECKPOINT_RAW_SIZE);
    for (uint32_t i = 0; i < CHECKPOINT_BLOCKS; i++)
        write_block(1 + i, buf + i * LOGFS_BLOCK_SIZE);
}

static void super_write_back(void)
{
    C.super.crc32 = 0;
    C.super.crc32 = lfs_crc32(&C.super,
                               sizeof(struct logfs_super) -
                               sizeof(uint32_t)); /* exclude crc32 field */
    uint8_t buf[LOGFS_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &C.super, sizeof(struct logfs_super));
    write_block(0, buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Inode bitmap helpers
   ═══════════════════════════════════════════════════════════════════════════ */

static void inode_bmap_set(uint64_t ino)
{
    if (ino < LOGFS_MAX_INODES)
        C.cp.inode_bitmap[ino / 8] |= (1U << (ino % 8));
}

static void inode_bmap_clear(uint64_t ino)
{
    if (ino < LOGFS_MAX_INODES)
        C.cp.inode_bitmap[ino / 8] &= ~(1U << (ino % 8));
}

static int inode_bmap_test(uint64_t ino)
{
    if (ino >= LOGFS_MAX_INODES) return 0;
    return (C.cp.inode_bitmap[ino / 8] >> (ino % 8)) & 1;
}

static int alloc_inode(void)
{
    for (uint64_t i = 0; i < LOGFS_MAX_INODES; i++) {
        if (!inode_bmap_test(i)) {
            inode_bmap_set(i);
            return (int)i;
        }
    }
    return -ENOSPC;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Path resolution: /foo/bar → (parent_ino, child_name, child_name_len)
   Returns 0 on success.  *parent_ino set to parent directory inode.
   If the path is "/" → parent_ino = 0, name = NULL.
   ═══════════════════════════════════════════════════════════════════════════ */

/* Find child named `name` (name_len bytes) in directory `dir_ino`.
   Returns the child inode number or -ENOENT. */
static int dir_lookup(uint64_t dir_ino, const char *name, size_t name_len)
{
    struct logfs_inode dir;
    int r = load_inode(dir_ino, &dir);
    if (r) return r;

    for (int d = 0; d < LOGFS_NDIRECT; d++) {
        if (dir.direct[d] == 0) continue;
        uint8_t blk[LOGFS_BLOCK_SIZE];
        r = read_block_cached(dir.direct[d], blk);
        if (r) return r;
        uint32_t off = 0;
        while (off + sizeof(struct logfs_dirent) <= LOGFS_BLOCK_SIZE) {
            struct logfs_dirent *de =
                (struct logfs_dirent *)(blk + off);
            if (de->ino == 0) {
                /* sentinel – rest of block is unused */
                if (de->name_len == 0 && de->name[0] == '\0')
                    break;
            }
            if (de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0)
                return (int)de->ino;
            off += sizeof(struct logfs_dirent);
        }
    }
    return -ENOENT;
}

/*
 * Split path into parent and final component.
 *   "/foo/bar"  → parent_path="/foo",  name="bar",  name_len=3
 *   "/foo"      → parent_path="/",      name="foo",  name_len=3
 *   "/"         → parent_path=NULL,     name=NULL,   name_len=0
 *
 * Caller must free *parent_path.
 * Returns 0 on success or -EINVAL on malformed path.
 */
static int split_path(const char *path, char **parent_path,
                      const char **name, size_t *name_len)
{
    if (!path || path[0] != '/') return -EINVAL;

    /* root special case */
    if (path[1] == '\0') {
        *parent_path = NULL;
        *name = NULL;
        *name_len = 0;
        return 0;
    }

    /* find last '/' */
    const char *last_slash = strrchr(path, '/');
    size_t nlen = strlen(last_slash + 1);
    if (nlen == 0 || nlen > LOGFS_MAX_FILENAME) return -EINVAL;

    *name = last_slash + 1;
    *name_len = nlen;

    if (last_slash == path) {
        /* parent is root */
        *parent_path = strdup("/");
    } else {
        size_t plen = (size_t)(last_slash - path);
        *parent_path = strndup(path, plen);
    }
    return *parent_path ? 0 : -ENOMEM;
}

/* Resolve a path to its inode number. */
static int path_resolve(const char *path, uint64_t *ino_out)
{
    if (!path || path[0] != '/') return -EINVAL;

    if (path[1] == '\0') {
        *ino_out = 0;
        return 0;
    }

    uint64_t cur = 0; /* start at root */
    char tmp[4096];
    strncpy(tmp, path + 1, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(tmp, "/", &saveptr);
    while (tok) {
        size_t tlen = strlen(tok);
        int child = dir_lookup(cur, tok, tlen);
        if (child < 0) return child;
        cur = (uint64_t)child;
        tok = strtok_r(NULL, "/", &saveptr);
    }
    *ino_out = cur;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Directory helpers
   ═══════════════════════════════════════════════════════════════════════════ */

/* Count live directory entries in a dir data block. */
static int count_dirents_in_block(uint64_t blk_no)
{
    uint8_t blk[LOGFS_BLOCK_SIZE];
    if (read_block_cached(blk_no, blk)) return 0;
    int count = 0;
    uint32_t off = 0;
    while (off + sizeof(struct logfs_dirent) <= LOGFS_BLOCK_SIZE) {
        struct logfs_dirent *de = (struct logfs_dirent *)(blk + off);
        if (de->ino == 0 && de->name_len == 0 &&
            de->name[0] == '\0')
            break;
        if (de->ino != 0)
            count++;
        off += sizeof(struct logfs_dirent);
    }
    return count;
}

/* Check whether directory `dir_ino` is empty. */
static int dir_is_empty(uint64_t dir_ino)
{
    struct logfs_inode dir;
    if (load_inode(dir_ino, &dir)) return 1; /* treat as empty on error */
    for (int d = 0; d < LOGFS_NDIRECT; d++) {
        if (dir.direct[d] == 0) continue;
        if (count_dirents_in_block(dir.direct[d]) > 0)
            return 0;
    }
    return 1;
}

/* Build the initial (empty) directory data block with a sentinel entry. */
static void build_empty_dir_block(uint8_t *blk)
{
    memset(blk, 0, LOGFS_BLOCK_SIZE);
    struct logfs_dirent sentinel;
    memset(&sentinel, 0, sizeof(sentinel));
    sentinel.ino      = 0;
    sentinel.name_len = 0;
    sentinel.name[0]  = '\0';
    memcpy(blk, &sentinel, sizeof(sentinel));
}

/* Add a directory entry to an existing directory's data blocks.
 * Allocates a new data block if needed.  Returns 0 or -ENOSPC. */
static int dir_add_entry(uint64_t dir_ino, const char *name,
                         size_t name_len, uint64_t child_ino)
{
    struct logfs_inode dir;
    int r = load_inode(dir_ino, &dir);
    if (r) return r;

    /* try to add to an existing block first */
    for (int d = 0; d < LOGFS_NDIRECT; d++) {
        if (dir.direct[d] == 0) continue;
        uint8_t blk[LOGFS_BLOCK_SIZE];
        r = read_block_cached(dir.direct[d], blk);
        if (r) return r;

        uint32_t off = 0;
        while (off + sizeof(struct logfs_dirent) <= LOGFS_BLOCK_SIZE) {
            struct logfs_dirent *de =
                (struct logfs_dirent *)(blk + off);
            if (de->ino == 0 && de->name_len == 0 &&
                de->name[0] == '\0') {
                /* found a free slot or sentinel – use it */
                de->ino      = child_ino;
                de->name_len = (uint16_t)name_len;
                memset(de->name, 0, LOGFS_MAX_FILENAME);
                memcpy(de->name, name, name_len);
                /* invalidate old block, insert new */
                data_cache_invalidate(dir.direct[d]);
                dc_insert(dir.direct[d], blk);
                return 0;
            }
            off += sizeof(struct logfs_dirent);
        }
    }

    /* need a new data block */
    if (dir.size >= (uint64_t)LOGFS_NDIRECT * LOGFS_BLOCK_SIZE)
        return -ENOSPC;

    int slot = (int)(dir.size / LOGFS_BLOCK_SIZE);
    uint8_t blk[LOGFS_BLOCK_SIZE];
    build_empty_dir_block(blk);
    struct logfs_dirent *de = (struct logfs_dirent *)blk;
    de->ino      = child_ino;
    de->name_len = (uint16_t)name_len;
    memset(de->name, 0, LOGFS_MAX_FILENAME);
    memcpy(de->name, name, name_len);

    /* We cannot know the final block number yet; the append_record will
       place the data block in the segment buffer.  Use block number 0
       as a placeholder – the real assignment happens during log append.
       However to keep it simple we will handle block allocation at the
       COW step. For now mark as needing allocation. */
    dir.direct[slot] = 0; /* will be filled during COW */
    dir.size += LOGFS_BLOCK_SIZE;
    save_inode(&dir);
    return 0;
}

/* Remove a directory entry.  Returns 0 or -ENOENT. */
static int dir_remove_entry(uint64_t dir_ino, const char *name,
                            size_t name_len)
{
    struct logfs_inode dir;
    int r = load_inode(dir_ino, &dir);
    if (r) return r;

    int found = 0;
    for (int d = 0; d < LOGFS_NDIRECT && !found; d++) {
        if (dir.direct[d] == 0) continue;
        uint8_t blk[LOGFS_BLOCK_SIZE];
        r = read_block_cached(dir.direct[d], blk);
        if (r) return r;

        uint32_t off = 0;
        while (off + sizeof(struct logfs_dirent) <= LOGFS_BLOCK_SIZE) {
            struct logfs_dirent *de =
                (struct logfs_dirent *)(blk + off);
            if (de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                /* clear this entry */
                de->ino = 0;
                de->name_len = 0;
                memset(de->name, 0, LOGFS_MAX_FILENAME);
                data_cache_invalidate(dir.direct[d]);
                dc_insert(dir.direct[d], blk);
                found = 1;
                break;
            }
            off += sizeof(struct logfs_dirent);
        }
    }
    return found ? 0 : -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Block allocator helpers for COW
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Allocate a fresh data block.  Returns the absolute block number on
 * success or -ENOSPC.  The block is placed in the write buffer and
 * the segment header's block_map is updated.
 *
 * `ino` is the inode that owns this block (for data blocks).
 */
static int64_t alloc_data_block(uint64_t ino)
{
    int r = wb_ensure_open();
    if (r) return r;
    uint64_t blk = LOG_START_BLOCK +
                   (uint64_t)C.wb.cur_seg * LOGFS_SEG_BLOCKS +
                   C.wb.next_slot;
    C.wb.next_slot++;
    /* mark the slot in segment header */
    struct logfs_segment_hdr *shdr =
        (struct logfs_segment_hdr *)C.wb.buf;
    shdr->block_map[C.wb.next_slot - 1] = (int32_t)ino;
    /* clear the slot in the buffer */
    memset(C.wb.buf + (C.wb.next_slot - 1) * LOGFS_BLOCK_SIZE,
           0, LOGFS_BLOCK_SIZE);
    return (int64_t)blk;
}

/*
 * Allocate a fresh inode block.  Returns the absolute block number or
 * -ENOSPC.
 */
static int64_t alloc_inode_block(uint64_t ino)
{
    int r = wb_ensure_open();
    if (r) return r;
    uint64_t blk = LOG_START_BLOCK +
                   (uint64_t)C.wb.cur_seg * LOGFS_SEG_BLOCKS +
                   C.wb.next_slot;
    C.wb.next_slot++;
    struct logfs_segment_hdr *shdr =
        (struct logfs_segment_hdr *)C.wb.buf;
    shdr->block_map[C.wb.next_slot - 1] = BLK_INODE;
    memset(C.wb.buf + (C.wb.next_slot - 1) * LOGFS_BLOCK_SIZE,
           0, LOGFS_BLOCK_SIZE);
    return (int64_t)blk;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Core COW record builder
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Append a COW record to the write buffer.  The caller provides the
 * assembled payload blocks and a matching block-map array.
 *
 * `payload` has `nblocks * LOGFS_BLOCK_SIZE` bytes.
 * `bmap[i]` = BLK_INODE or (int)inode_number.
 *
 * This function handles:
 *   1. Flushing the segment buffer if the record doesn't fit.
 *   2. Writing the record header and payload into the buffer.
 *   3. Updating imap for any inode blocks in the record.
 *
 * Returns 0 on success or a negative errno.
 */
static int cow_append(uint32_t op, const uint8_t *payload,
                      uint32_t nblocks, const int32_t *bmap)
{
    uint32_t total = 1 + nblocks;

    /* flush if this record won't fit in the current segment */
    if (C.wb.active &&
        C.wb.next_slot + (int)total > LOGFS_SEG_BLOCKS) {
        int r = wb_flush();
        if (r) return r;
    }

    /* allocate a new segment if we don't have one */
    if (!C.wb.active || C.wb.next_slot == 0) {
        int s = alloc_seg();
        if (s < 0) return s;
        wb_init((uint32_t)s);
    }

    uint32_t slot = C.wb.next_slot;

    /* record header */
    struct logfs_rec_hdr rhdr;
    memset(&rhdr, 0, sizeof(rhdr));
    rhdr.magic   = REC_MAGIC;
    rhdr.op      = op;
    rhdr.nblocks = nblocks;
    set_crc(&rhdr, sizeof(rhdr));
    memcpy(C.wb.buf + slot * LOGFS_BLOCK_SIZE, &rhdr, sizeof(rhdr));

    /* mark the header slot in segment header */
    struct logfs_segment_hdr *shdr =
        (struct logfs_segment_hdr *)C.wb.buf;
    shdr->block_map[slot] = BLK_INODE;

    /* payload blocks */
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t ps = slot + 1 + i;
        memcpy(C.wb.buf + ps * LOGFS_BLOCK_SIZE,
               payload + i * LOGFS_BLOCK_SIZE,
               LOGFS_BLOCK_SIZE);
        shdr->block_map[ps] = bmap[i];
    }

    C.wb.next_slot += total;

    /* update imap for inode blocks */
    for (uint32_t i = 0; i < nblocks; i++) {
        if (bmap[i] == BLK_INODE) {
            const struct logfs_inode *ip =
                (const struct logfs_inode *)(payload +
                    i * LOGFS_BLOCK_SIZE);
            uint64_t newblk = LOG_START_BLOCK +
                              (uint64_t)C.wb.cur_seg * LOGFS_SEG_BLOCKS +
                              slot + 1 + i;
            imap_set(ip->ino, newblk);
            /* invalidate old cache copies */
            inode_cache_invalidate(ip->ino);
            ic_insert(ip->ino, ip);
        } else if (bmap[i] >= 0) {
            uint64_t newblk = LOG_START_BLOCK +
                              (uint64_t)C.wb.cur_seg * LOGFS_SEG_BLOCKS +
                              slot + 1 + i;
            data_cache_invalidate(newblk);
            dc_insert(newblk, payload + i * LOGFS_BLOCK_SIZE);
        }
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Garbage collection  (Task D)
   ═══════════════════════════════════════════════════════════════════════════ */

/* How many log segments are currently in use (rough count). */
static uint32_t log_usage_segments(void)
{
    uint32_t count = 0;
    for (uint32_t s = 0; s < LOGFS_MAX_SEGMENTS; s++)
        if (C.cp.segment_bitmap[s]) count++;
    return count;
}

/* Check whether a data block is still reachable. */
static int block_is_live(uint64_t blk, int32_t bmap_val)
{
    if (bmap_val == BLK_INODE) {
        /* inode block – live if imap points here */
        for (uint64_t i = 0; i < LOGFS_MAX_INODES; i++)
            if (C.cp.imap[i] == blk) return 1;
        return 0;
    }
    if (bmap_val < 0) return 0; /* free or sentinel */
    /* data block – live if some inode's direct[] points here */
    uint64_t ino = (uint64_t)bmap_val;
    struct logfs_inode ip;
    if (load_inode(ino, &ip) == 0) {
        for (int d = 0; d < LOGFS_NDIRECT; d++)
            if (ip.direct[d] == blk) return 1;
    }
    return 0;
}

int lfs_gc(void)
{
    /* Choose candidates: sealed segments with live_ratio < 25%,
       or any sealed segment if usage > 80%. */
    uint32_t max_seg = LOGFS_MAX_SEGMENTS;
    uint32_t usage   = log_usage_segments();

    for (uint32_t s = 0; s < max_seg; s++) {
        if (!C.cp.segment_bitmap[s]) continue;
        if (s == C.wb.cur_seg && C.wb.active) continue; /* skip open seg */

        uint64_t seg_start = LOG_START_BLOCK +
                             (uint64_t)s * LOGFS_SEG_BLOCKS;

        /* read the segment header */
        struct logfs_segment_hdr shdr;
        if (read_block(seg_start, &shdr)) continue;
        if (shdr.magic != SEG_MAGIC) continue;
        if (!(shdr.flags & 1)) continue; /* not sealed */

        /* compute live ratio */
        uint32_t mapped = 0, live = 0;
        for (int i = 1; i < LOGFS_SEG_BLOCKS; i++) {
            if (shdr.block_map[i] != BLK_FREE) {
                mapped++;
                if (block_is_live(seg_start + i, shdr.block_map[i]))
                    live++;
            }
        }
        if (mapped == 0) {
            /* empty segment – just free it */
            C.cp.segment_bitmap[s] = 0;
            continue;
        }
        float ratio = (float)live / (float)mapped;
        int collect = (ratio < 0.25f) ||
                      (usage * LOGFS_SEG_BLOCKS * LOGFS_BLOCK_SIZE >
                       (uint64_t)(LOGFS_IMAGE_SIZE * 0.8));
        if (!collect) continue;

        /* copy live blocks forward */
        for (int i = 1; i < LOGFS_SEG_BLOCKS; i++) {
            if (shdr.block_map[i] == BLK_FREE) continue;
            if (!block_is_live(seg_start + i, shdr.block_map[i]))
                continue;

            uint8_t blk_data[LOGFS_BLOCK_SIZE];
            if (read_block(seg_start + i, blk_data)) continue;

            int32_t bmap_val = shdr.block_map[i];
            int32_t new_bmap[1] = { bmap_val };
            int r = cow_append(OP_WRITE, blk_data, 1, new_bmap);
            if (r) return r;
        }

        /* free the old segment */
        C.cp.segment_bitmap[s] = 0;
    }

    /* flush checkpoint after GC */
    cp_write_back();
    super_write_back();
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Library API – mount / umount
   ═══════════════════════════════════════════════════════════════════════════ */

int lfs_mount(const char *img_path)
{
    if (!crc_ready) crc_init();

    C.fd = open(img_path, O_RDWR);
    if (C.fd < 0) return -errno;

    /* ── superblock ───────────────────────────────────────────────────── */
    uint8_t blk[LOGFS_BLOCK_SIZE];
    if (read_block(0, blk)) { close(C.fd); return -EIO; }
    memcpy(&C.super, blk, sizeof(struct logfs_super));
    if (C.super.magic != LOGFS_MAGIC ||
        C.super.block_size != LOGFS_BLOCK_SIZE ||
        verify_crc(&C.super,
                   sizeof(struct logfs_super) - sizeof(uint32_t))) {
        close(C.fd);
        return -EIO;
    }

    /* ── checkpoint ───────────────────────────────────────────────────── */
    memset(&C.cp, 0, sizeof(C.cp));
    uint8_t cp_buf[CHECKPOINT_BLOCKS * LOGFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < CHECKPOINT_BLOCKS; i++)
        read_block(1 + i, cp_buf + i * LOGFS_BLOCK_SIZE);
    memcpy(&C.cp, cp_buf, CHECKPOINT_RAW_SIZE);
    if (verify_crc(&C.cp, CHECKPOINT_RAW_SIZE)) {
        close(C.fd);
        return -EIO;
    }

    /* ── caches ───────────────────────────────────────────────────────── */
    dc_init();
    ic_init();

    /* ── load root inode into cache ───────────────────────────────────── */
    struct logfs_inode root;
    if (load_inode(0, &root)) {
        close(C.fd);
        return -EIO;
    }

    /* ── reconstruct append tip ───────────────────────────────────────── */
    C.next_seg_seq = 0;
    C.wb.active    = 0;
    int found = 0;
    for (uint32_t s = 0; s < LOGFS_MAX_SEGMENTS; s++) {
        if (!C.cp.segment_bitmap[s]) continue;
        uint64_t seg_start = LOG_START_BLOCK +
                             (uint64_t)s * LOGFS_SEG_BLOCKS;
        struct logfs_segment_hdr shdr;
        if (read_block(seg_start, &shdr)) continue;
        if (shdr.magic != SEG_MAGIC) continue;
        if (!(shdr.flags & 1)) {
            /* open segment – find first free slot */
            if (shdr.seg_seq >= C.next_seg_seq)
                C.next_seg_seq = shdr.seg_seq + 1;
            wb_init(s);
            /* restore the header from disk */
            memcpy(C.wb.buf, &shdr, sizeof(shdr));
            /* find next free slot */
            for (int i = 1; i < LOGFS_SEG_BLOCKS; i++) {
                if (shdr.block_map[i] == BLK_FREE) {
                    C.wb.next_slot = i;
                    break;
                }
            }
            found = 1;
            break;
        } else {
            if (shdr.seg_seq >= C.next_seg_seq)
                C.next_seg_seq = shdr.seg_seq + 1;
        }
    }
    if (!found) {
        /* no open segment – allocate a fresh one */
        int s = alloc_seg();
        if (s >= 0) wb_init((uint32_t)s);
    }

    C.mounted = 1;
    strncpy(C.img_path, img_path, sizeof(C.img_path) - 1);
    return 0;
}

int lfs_umount(void)
{
    if (!C.mounted) return -EINVAL;

    /* flush segment write buffer */
    wb_flush();

    /* write back dirty data cache entries */
    for (int i = 0; i < DATA_CACHE_SIZE; i++) {
        if (C.data_cache.entries[i].valid && C.data_cache.entries[i].dirty) {
            off_t off = (off_t)C.data_cache.entries[i].block_no *
                        LOGFS_BLOCK_SIZE;
            pwrite(C.fd, C.data_cache.entries[i].data,
                   LOGFS_BLOCK_SIZE, off);
            C.data_cache.entries[i].dirty = 0;
        }
    }

    cp_write_back();
    super_write_back();
    fsync(C.fd);
    close(C.fd);
    C.mounted = 0;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Library API – updates (COW)
   ═══════════════════════════════════════════════════════════════════════════ */

int lfs_create(const char *path, mode_t mode)
{
    if (!path || path[0] != '/') return -EINVAL;

    char *ppath = NULL;
    const char *cname;
    size_t clen;
    int r = split_path(path, &ppath, &cname, &clen);
    if (r) return r;
    if (!cname) return -EINVAL; /* can't create "/" */

    /* resolve parent */
    uint64_t parent_ino;
    r = path_resolve(ppath ? ppath : "/", &parent_ino);
    free(ppath);
    if (r) return r;

    /* check child doesn't already exist */
    r = dir_lookup(parent_ino, cname, clen);
    if (r != -ENOENT) return (r == 0) ? -EEXIST : r;

    /* allocate new inode */
    int new_ino = alloc_inode();
    if (new_ino < 0) return new_ino;

    /* build new inode */
    struct logfs_inode nino;
    memset(&nino, 0, sizeof(nino));
    nino.ino  = (uint64_t)new_ino;
    nino.mode = S_IFREG | (mode & 0777);
    nino.size = 0;
    nino.mtime = time(NULL);
    for (int i = 0; i < LOGFS_NDIRECT; i++) nino.direct[i] = 0;

    /* build parent directory data with the new entry added */
    struct logfs_inode parent;
    r = load_inode(parent_ino, &parent);
    if (r) return r;

    /* We need to assemble the new parent dir data blocks with the new
       entry.  Strategy: load each existing parent data block, add the
       entry (or use sentinel slot), and COW all parent blocks + new
       child inode + updated parent inode in a single record. */

    /* Count existing parent data blocks and total dirent capacity. */
    int parent_dblk_count = 0;
    for (int d = 0; d < LOGFS_NDIRECT; d++)
        if (parent.direct[d]) parent_dblk_count++;

    /* Try to find a free slot in existing parent blocks. */
    int added = 0;
    uint8_t parent_blks[LOGFS_NDIRECT][LOGFS_BLOCK_SIZE];
    uint64_t parent_blk_nos[LOGFS_NDIRECT];
    for (int d = 0; d < LOGFS_NDIRECT; d++) {
        if (parent.direct[d]) {
            parent_blk_nos[d] = parent.direct[d];
            r = read_block_cached(parent.direct[d], parent_blks[d]);
            if (r) return r;
        } else {
            parent_blk_nos[d] = 0;
            memset(parent_blks[d], 0, LOGFS_BLOCK_SIZE);
        }
    }

    /* find or create free dirent slot */
    for (int d = 0; d < LOGFS_NDIRECT && !added; d++) {
        if (parent.direct[d] == 0) continue;
        uint32_t off = 0;
        while (off + sizeof(struct logfs_dirent) <= LOGFS_BLOCK_SIZE) {
            struct logfs_dirent *de =
                (struct logfs_dirent *)(parent_blks[d] + off);
            if (de->ino == 0 && de->name_len == 0 &&
                de->name[0] == '\0') {
                de->ino      = (uint64_t)new_ino;
                de->name_len = (uint16_t)clen;
                memset(de->name, 0, LOGFS_MAX_FILENAME);
                memcpy(de->name, cname, clen);
                added = 1;
                break;
            }
            off += sizeof(struct logfs_dirent);
        }
    }

    if (!added) {
        /* need a new parent data block */
        int slot = parent_dblk_count;
        if (slot >= LOGFS_NDIRECT) return -ENOSPC;
        build_empty_dir_block(parent_blks[slot]);
        struct logfs_dirent *de = (struct logfs_dirent *)parent_blks[slot];
        de->ino      = (uint64_t)new_ino;
        de->name_len = (uint16_t)clen;
        memset(de->name, 0, LOGFS_MAX_FILENAME);
        memcpy(de->name, cname, clen);
        parent_blk_nos[slot] = 0; /* will be allocated by COW */
        parent.direct[slot] = 1; /* mark slot used (will get real block) */
        parent.size += LOGFS_BLOCK_SIZE;
    }

    /* ── assemble the COW record payload ────────────────────────────── */
    /*
     * Record layout: [new child inode] [parent dir blk 0..N] [parent inode]
     * nblocks = 1 + parent_dblk_count_or_new + 1
     */
    int new_parent_dblk_count = parent_dblk_count;
    if (!added) new_parent_dblk_count++;

    uint32_t total_blocks = 1 + (uint32_t)new_parent_dblk_count + 1;
    uint8_t *payload = calloc(total_blocks, LOGFS_BLOCK_SIZE);
    int32_t *bmap    = calloc(total_blocks, sizeof(int32_t));
    if (!payload || !bmap) { free(payload); free(bmap); return -ENOMEM; }

    /* block 0: new child inode */
    memcpy(payload, &nino, sizeof(nino));
    bmap[0] = BLK_INODE;

    /* blocks 1..N: parent directory data */
    for (int d = 0; d < new_parent_dblk_count; d++) {
        memcpy(payload + (1 + d) * LOGFS_BLOCK_SIZE,
               parent_blks[d], LOGFS_BLOCK_SIZE);
        bmap[1 + d] = (int32_t)parent_ino;
    }

    /* last block: updated parent inode (size may have changed) */
    memcpy(payload + (1 + new_parent_dblk_count) * LOGFS_BLOCK_SIZE,
           &parent, sizeof(parent));
    bmap[1 + new_parent_dblk_count] = BLK_INODE;

    /* append the COW record */
    r = cow_append(OP_CREATE, payload, total_blocks, bmap);
    free(payload);
    free(bmap);
    if (r) return r;

    /* run GC if log usage is high */
    if (log_usage_segments() * LOGFS_SEG_BLOCKS * LOGFS_BLOCK_SIZE >
        (uint64_t)(LOGFS_IMAGE_SIZE * 0.8))
        lfs_gc();

    return 0;
}

int lfs_mkdir(const char *path, mode_t mode)
{
    if (!path || path[0] != '/') return -EINVAL;

    char *ppath = NULL;
    const char *cname;
    size_t clen;
    int r = split_path(path, &ppath, &cname, &clen);
    if (r) return r;
    if (!cname) return -EINVAL;

    uint64_t parent_ino;
    r = path_resolve(ppath ? ppath : "/", &parent_ino);
    free(ppath);
    if (r) return r;

    r = dir_lookup(parent_ino, cname, clen);
    if (r != -ENOENT) return (r == 0) ? -EEXIST : r;

    int new_ino = alloc_inode();
    if (new_ino < 0) return new_ino;

    /* build new directory inode with one empty data block */
    struct logfs_inode nino;
    memset(&nino, 0, sizeof(nino));
    nino.ino  = (uint64_t)new_ino;
    nino.mode = S_IFDIR | (mode & 0777);
    nino.size = LOGFS_BLOCK_SIZE; /* one empty data block */
    nino.mtime = time(NULL);

    /* We need to COW: [new dir inode] [dir data block] [parent dir blocks]
       [parent inode] */
    struct logfs_inode parent;
    r = load_inode(parent_ino, &parent);
    if (r) return r;

    int parent_dblk_count = 0;
    for (int d = 0; d < LOGFS_NDIRECT; d++)
        if (parent.direct[d]) parent_dblk_count++;

    uint8_t parent_blks[LOGFS_NDIRECT][LOGFS_BLOCK_SIZE];
    uint64_t parent_blk_nos[LOGFS_NDIRECT];
    for (int d = 0; d < LOGFS_NDIRECT; d++) {
        if (parent.direct[d]) {
            parent_blk_nos[d] = parent.direct[d];
            r = read_block_cached(parent.direct[d], parent_blks[d]);
            if (r) return r;
        } else {
            parent_blk_nos[d] = 0;
            memset(parent_blks[d], 0, LOGFS_BLOCK_SIZE);
        }
    }

    /* add entry to parent */
    int added = 0;
    for (int d = 0; d < LOGFS_NDIRECT && !added; d++) {
        if (parent.direct[d] == 0) continue;
        uint32_t off = 0;
        while (off + sizeof(struct logfs_dirent) <= LOGFS_BLOCK_SIZE) {
            struct logfs_dirent *de =
                (struct logfs_dirent *)(parent_blks[d] + off);
            if (de->ino == 0 && de->name_len == 0 &&
                de->name[0] == '\0') {
                de->ino      = (uint64_t)new_ino;
                de->name_len = (uint16_t)clen;
                memset(de->name, 0, LOGFS_MAX_FILENAME);
                memcpy(de->name, cname, clen);
                added = 1;
                break;
            }
            off += sizeof(struct logfs_dirent);
        }
    }
    if (!added) {
        int slot = parent_dblk_count;
        if (slot >= LOGFS_NDIRECT) return -ENOSPC;
        build_empty_dir_block(parent_blks[slot]);
        struct logfs_dirent *de = (struct logfs_dirent *)parent_blks[slot];
        de->ino      = (uint64_t)new_ino;
        de->name_len = (uint16_t)clen;
        memset(de->name, 0, LOGFS_MAX_FILENAME);
        memcpy(de->name, cname, clen);
        parent_blk_nos[slot] = 0;
        parent.direct[slot] = 1;
        parent.size += LOGFS_BLOCK_SIZE;
        parent_dblk_count++;
    }

    int new_parent_dblk = parent_dblk_count;

    /* payload: [new dir inode] [new dir data block] [parent blk 0..N] [parent inode] */
    uint32_t total_blocks = 1 + 1 + (uint32_t)new_parent_dblk + 1;
    uint8_t *payload = calloc(total_blocks, LOGFS_BLOCK_SIZE);
    int32_t *bmap    = calloc(total_blocks, sizeof(int32_t));
    if (!payload || !bmap) { free(payload); free(bmap); return -ENOMEM; }

    /* new dir inode */
    memcpy(payload, &nino, sizeof(nino));
    bmap[0] = BLK_INODE;

    /* new dir empty data block */
    uint8_t empty_dir[LOGFS_BLOCK_SIZE];
    build_empty_dir_block(empty_dir);
    memcpy(payload + LOGFS_BLOCK_SIZE, empty_dir, LOGFS_BLOCK_SIZE);
    bmap[1] = (int32_t)new_ino;

    /* parent dir blocks */
    for (int d = 0; d < new_parent_dblk; d++) {
        memcpy(payload + (2 + d) * LOGFS_BLOCK_SIZE,
               parent_blks[d], LOGFS_BLOCK_SIZE);
        bmap[2 + d] = (int32_t)parent_ino;
    }

    /* parent inode */
    memcpy(payload + (2 + new_parent_dblk) * LOGFS_BLOCK_SIZE,
           &parent, sizeof(parent));
    bmap[2 + new_parent_dblk] = BLK_INODE;

    r = cow_append(OP_MKDIR, payload, total_blocks, bmap);
    free(payload);
    free(bmap);
    if (r) return r;

    if (log_usage_segments() * LOGFS_SEG_BLOCKS * LOGFS_BLOCK_SIZE >
        (uint64_t)(LOGFS_IMAGE_SIZE * 0.8))
        lfs_gc();

    return 0;
}

int lfs_rmdir(const char *path)
{
    if (!path || path[0] != '/') return -EINVAL;

    char *ppath = NULL;
    const char *cname;
    size_t clen;
    int r = split_path(path, &ppath, &cname, &clen);
    if (r) return r;
    if (!cname) return -EINVAL; /* can't rmdir "/" */

    uint64_t parent_ino;
    r = path_resolve(ppath ? ppath : "/", &parent_ino);
    free(ppath);
    if (r) return r;

    int child_ino_val = dir_lookup(parent_ino, cname, clen);
    if (child_ino_val < 0) return child_ino_val;
    uint64_t child_ino = (uint64_t)child_ino_val;

    /* must be a directory */
    struct logfs_inode child_ip;
    r = load_inode(child_ino, &child_ip);
    if (r) return r;
    if (!S_ISDIR(child_ip.mode)) return -ENOTDIR;

    /* must be empty */
    if (!dir_is_empty(child_ino)) return -ENOTEMPTY;

    /* remove entry from parent directory */
    struct logfs_inode parent;
    r = load_inode(parent_ino, &parent);
    if (r) return r;

    int parent_dblk_count = 0;
    uint8_t parent_blks[LOGFS_NDIRECT][LOGFS_BLOCK_SIZE];
    for (int d = 0; d < LOGFS_NDIRECT; d++) {
        if (parent.direct[d]) {
            parent_dblk_count++;
            r = read_block_cached(parent.direct[d], parent_blks[d]);
            if (r) return r;
            /* remove the entry */
            uint32_t off = 0;
            while (off + sizeof(struct logfs_dirent) <= LOGFS_BLOCK_SIZE) {
                struct logfs_dirent *de =
                    (struct logfs_dirent *)(parent_blks[d] + off);
                if (de->name_len == clen &&
                    memcmp(de->name, cname, clen) == 0) {
                    de->ino = 0;
                    de->name_len = 0;
                    memset(de->name, 0, LOGFS_MAX_FILENAME);
                    break;
                }
                off += sizeof(struct logfs_dirent);
            }
        }
    }

    /* clear child inode in imap */
    inode_bmap_clear(child_ino);
    imap_set(child_ino, 0);
    inode_cache_invalidate(child_ino);

    /* COW: [parent dir blocks] [parent inode] */
    uint32_t total_blocks = (uint32_t)parent_dblk_count + 1;
    uint8_t *payload = calloc(total_blocks, LOGFS_BLOCK_SIZE);
    int32_t *bmap    = calloc(total_blocks, sizeof(int32_t));
    if (!payload || !bmap) { free(payload); free(bmap); return -ENOMEM; }

    int bi = 0;
    for (int d = 0; d < LOGFS_NDIRECT; d++) {
        if (parent.direct[d]) {
            memcpy(payload + bi * LOGFS_BLOCK_SIZE,
                   parent_blks[d], LOGFS_BLOCK_SIZE);
            bmap[bi] = (int32_t)parent_ino;
            bi++;
        }
    }
    memcpy(payload + bi * LOGFS_BLOCK_SIZE, &parent, sizeof(parent));
    bmap[bi] = BLK_INODE;

    r = cow_append(OP_RMDIR, payload, total_blocks, bmap);
    free(payload);
    free(bmap);
    return r;
}

int lfs_write(const char *path, const char *buf, size_t size, off_t offset)
{
    if (!path || !buf || size == 0) return 0;

    uint64_t ino;
    int r = path_resolve(path, &ino);
    if (r) return r;

    struct logfs_inode ip;
    r = load_inode(ino, &ip);
    if (r) return r;

    /* For simplicity, we handle single-block writes.  The assignment says
       larger files are optional bonus.  We support writes within the
       NDIRECT blocks. */

    size_t end = (size_t)(offset + size);
    if (end > (size_t)LOGFS_NDIRECT * LOGFS_BLOCK_SIZE)
        return -EFBIG;

    /* figure out which direct blocks are affected */
    int first_blk = (int)(offset / LOGFS_BLOCK_SIZE);
    int last_blk  = (int)((offset + size - 1) / LOGFS_BLOCK_SIZE);

    /* We will COW the new data blocks and the updated inode.
       For simplicity, re-COW every touched block (even partially written). */
    int num_data = last_blk - first_blk + 1;
    int total_blocks = num_data + 1; /* data blocks + inode */

    uint8_t *payload = calloc(total_blocks, LOGFS_BLOCK_SIZE);
    int32_t *bmap    = calloc(total_blocks, sizeof(int32_t));
    if (!payload || !bmap) { free(payload); free(bmap); return -ENOMEM; }

    for (int i = 0; i < num_data; i++) {
        int blk_idx = first_blk + i;
        uint8_t *blk_data = payload + i * LOGFS_BLOCK_SIZE;

        /* load existing block content (or zero) */
        if (ip.direct[blk_idx] != 0)
            read_block_cached(ip.direct[blk_idx], blk_data);

        /* compute the byte range within this block */
        size_t blk_start = (size_t)blk_idx * LOGFS_BLOCK_SIZE;
        size_t blk_end   = blk_start + LOGFS_BLOCK_SIZE;
        size_t w_start   = (offset > blk_start) ? offset : blk_start;
        size_t w_end     = ((offset + size) < blk_end) ?
                           (offset + size) : blk_end;

        memcpy(blk_data + (w_start - blk_start),
               buf + (w_start - offset),
               w_end - w_start);

        /* update the direct pointer – will be set in the new inode below */
        ip.direct[blk_idx] = 0; /* mark as needing new block */
        bmap[i] = (int32_t)ino;
    }

    /* update size and mtime */
    if (offset + (off_t)size > (off_t)ip.size)
        ip.size = (uint64_t)(offset + size);
    ip.mtime = time(NULL);

    /* inode block (last) */
    memcpy(payload + num_data * LOGFS_BLOCK_SIZE, &ip, sizeof(ip));
    bmap[num_data] = BLK_INODE;

    r = cow_append(OP_WRITE, payload, total_blocks, bmap);
    free(payload);
    free(bmap);
    if (r) return r;

    if (log_usage_segments() * LOGFS_SEG_BLOCKS * LOGFS_BLOCK_SIZE >
        (uint64_t)(LOGFS_IMAGE_SIZE * 0.8))
        lfs_gc();

    return (int)size;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Library API – reads (no log records)
   ═══════════════════════════════════════════════════════════════════════════ */

int lfs_lookup(const char *path, uint64_t *ino_out)
{
    if (!path || !ino_out) return -EINVAL;
    return path_resolve(path, ino_out);
}

int lfs_getattr(const char *path, struct stat *stbuf)
{
    if (!path || !stbuf) return -EINVAL;

    uint64_t ino;
    int r = path_resolve(path, &ino);
    if (r) return r;

    struct logfs_inode ip;
    r = load_inode(ino, &ip);
    if (r) return r;

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_ino  = ip.ino;
    stbuf->st_mode  = ip.mode;
    stbuf->st_size  = ip.size;
    stbuf->st_mtime = ip.mtime;
    stbuf->st_nlink = 1;
    stbuf->st_blksize = LOGFS_BLOCK_SIZE;
    if (S_ISDIR(ip.mode))
        stbuf->st_nlink = 2;
    return 0;
}

int lfs_readdir(const char *path, void *buf, lfs_dir_filler_t filler)
{
    if (!path || !filler) return -EINVAL;

    uint64_t ino;
    int r = path_resolve(path, &ino);
    if (r) return r;

    struct logfs_inode dir;
    r = load_inode(ino, &dir);
    if (r) return r;
    if (!S_ISDIR(dir.mode)) return -ENOTDIR;

    for (int d = 0; d < LOGFS_NDIRECT; d++) {
        if (dir.direct[d] == 0) continue;
        uint8_t blk[LOGFS_BLOCK_SIZE];
        r = read_block_cached(dir.direct[d], blk);
        if (r) return r;

        uint32_t off = 0;
        while (off + sizeof(struct logfs_dirent) <= LOGFS_BLOCK_SIZE) {
            struct logfs_dirent *de =
                (struct logfs_dirent *)(blk + off);
            if (de->ino == 0 && de->name_len == 0 &&
                de->name[0] == '\0')
                break;
            if (de->ino != 0) {
                /* get mode from child inode for filler */
                struct logfs_inode child;
                if (load_inode(de->ino, &child) == 0)
                    filler(buf, de->name, de->ino, child.mode);
                else
                    filler(buf, de->name, de->ino, 0);
            }
            off += sizeof(struct logfs_dirent);
        }
    }
    return 0;
}

int lfs_read(const char *path, char *buf, size_t size, off_t offset)
{
    if (!path || !buf || size == 0) return 0;

    uint64_t ino;
    int r = path_resolve(path, &ino);
    if (r) return r;

    struct logfs_inode ip;
    r = load_inode(ino, &ip);
    if (r) return r;

    /* clamp to file size */
    if ((uint64_t)offset >= ip.size) return 0;
    if (offset + (off_t)size > (off_t)ip.size)
        size = (size_t)((off_t)ip.size - offset);

    size_t bytes_read = 0;
    while (size > 0) {
        int blk_idx = (int)(offset / LOGFS_BLOCK_SIZE);
        if (blk_idx >= LOGFS_NDIRECT) break;

        uint64_t blk_no = ip.direct[blk_idx];
        uint8_t blk_data[LOGFS_BLOCK_SIZE];
        if (blk_no == 0) {
            memset(blk_data, 0, LOGFS_BLOCK_SIZE);
        } else {
            r = read_block_cached(blk_no, blk_data);
            if (r) return r;
        }

        size_t blk_off  = offset % LOGFS_BLOCK_SIZE;
        size_t to_copy  = LOGFS_BLOCK_SIZE - blk_off;
        if (to_copy > size) to_copy = size;

        memcpy(buf + bytes_read, blk_data + blk_off, to_copy);
        bytes_read += to_copy;
        offset     += (off_t)to_copy;
        size       -= to_copy;
    }
    return (int)bytes_read;
}
