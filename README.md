 Log-Structured Filesystem with FUSE

## Build & Run

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install build-essential libfuse3-dev pkg-config

# macOS (FUSE via macFUSE + libfuse3)
# install macFUSE from https://osxfuse.github.io/
# then: brew install pkg-config
# note: macOS uses osxfuse; the Makefile flags may need -I/usr/local/include -L/usr/local/lib -lfuse3
```

### Build

```bash
make            # builds mkfs.logfs, test_driver, mount.logfs
make clean      # removes build artifacts and logfs.img
```

### Phase 1 (no FUSE)

```bash
./mkfs.logfs              # creates logfs.img
./test_driver logfs.img   # runs Phase 1 tests
```

### Phase 2 (FUSE)

```bash
./mkfs.logfs
mkdir -p /mnt/logfs
./mount.logfs -f /mnt/logfs        # foreground mount
# or: ./mount.logfs -f -o img=logfs.img /mnt/logfs

ls /mnt/logfs
mkdir /mnt/logfs/docs
echo hello > /mnt/logfs/docs/a.txt
cat /mnt/logfs/docs/a.txt
fusermount3 -u /mnt/logfs          # unmount
```

## Disk Layout

| Block(s)         | Region                    |
|------------------|---------------------------|
| 0                | Superblock                |
| 1 – 3           | Checkpoint (imap + bitmaps)|
| 4 onward         | Log segments (64 blocks each)|

- Image size: 256 MiB  (65536 blocks)
- Block size: 4096 bytes
- Segment size: 64 blocks (256 KiB)
- Max inodes: 1024
- Root inode: 0

## Cache Design

### Data Block Cache
- 64 entries, direct-mapped with LRU eviction
- Each entry: block number + 4096-byte data + valid/dirty flags
- Reads check cache first, then write buffer, then disk
- Eviction flushes dirty entries to disk

### Inode Cache
- 64 entries, LRU eviction
- Each entry: inode number + logfs_inode + valid/dirty flags

### Checkpoint Cache
- Full checkpoint (imap + inode bitmap + segment bitmap) kept in memory at all times
- Written back on flush, umount, and GC completion

### Superblock Cache
- Single in-memory copy; written back on flush and umount

### Segment Write Buffer
- One buffer = 64 blocks (256 KiB)
- Each log record is appended sequentially (header + payload blocks)
- Flushes to disk when full; on flush the segment is sealed and a new segment is allocated from the segment bitmap

## Flush / Write-Back Policy

- **Segment buffer**: flushed when full, or explicitly before a new segment is needed
- **Checkpoint + Superblock**: written back on umount and after GC
- **Dirty data cache entries**: flushed to disk on eviction and on umount
- After each successful fsync on logfs.img (segment flush or umount), the library writes "1" to `/sys/kernel/logfs_stats/notify` if that file exists (see Task F); if not present the write is silently skipped

## Eviction Policy

- **Data cache**: LRU with 64 entries; on eviction, dirty entries are written to disk
- **Inode cache**: LRU with 64 entries; dirty entries update the cache copy only (the on-disk copy is always a fresh COW write)
- **Checkpoint**: not evicted (always in memory)

## Garbage Collection Policy

- **Trigger**: when log usage exceeds 80% of available log space, GC runs automatically after any update operation
- **Candidates**: sealed segments whose live ratio is strictly below 25%
- **Live ratio** = (number of blocks that are live and mapped) / (number of blocks that are mapped)
- **Live block detection**:
  - Inode block (`block_map[i] == -2`): live if `imap[ino]` points to it
  - Data block (`block_map[i] = ino`): live if some `direct[]` of that inode still references it
- **Action**: copy live blocks forward via COW into the current write buffer, update imap, free the old segment (clear its bit in the segment bitmap)
- GC is also exposed as `lfs_gc()` for test-driver use
- GC never collects the open write buffer

## File

```
logfs.img        # 256 MiB backing file (created by mkfs.logfs)
```

## Task F – Kernel Module (logfs_stats)

The `logfs_stats.ko` kernel module (Task F) creates:
- `/sys/kernel/logfs_stats/flush_count` – read-only counter
- `/sys/kernel/logfs_stats/notify` – write "1" to increment the counter

The library calls this after every fsync on logfs.img. If the sysfs file does not exist the write is silently skipped.

*Note: the kernel module source is not included here; it is a separate kernel-level deliverable.*
