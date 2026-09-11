/* mount.logfs.c  –  FUSE mount program (Task E) */

#define FUSE_USE_VERSION 35
#include "lfs.h"

#include <fuse3/fuse.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ── helper: lfs_dir_filler → fuse_fill_dir_t adapter ────────────────── */

struct fuse_dir_data {
    fuse_fill_dir_t filler;
    void *buf;
};

static int fuse_dir_filler(void *buf, const char *name,
                           uint64_t ino, mode_t mode)
{
    struct fuse_dir_data *d = buf;
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino  = ino;
    st.st_mode = mode;
    return d->fill(d->buf, name, &st, 0, 0);
}

/* ── FUSE callbacks ─────────────────────────────────────────────────── */

static int lfs_fuse_getattr(const char *path, struct stat *stbuf,
                            struct fuse_file_info *fi)
{
    (void)fi;
    int r = lfs_getattr(path, stbuf);
    return (r < 0) ? r : 0;
}

static int lfs_fuse_readdir(const char *path, void *buf,
                            fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info *fi,
                            enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi; (void)flags;
    struct fuse_dir_data d = { .fill = filler, .buf = buf };
    return lfs_readdir(path, &d, fuse_dir_filler);
}

static int lfs_fuse_open(const char *path, struct fuse_file_info *fi)
{
    /* just check the file exists */
    struct stat st;
    int r = lfs_getattr(path, &st);
    if (r) return r;
    if (S_ISDIR(st.st_mode)) return -EISDIR;
    return 0;
}

static int lfs_fuse_read(const char *path, char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    return lfs_read(path, buf, size, offset);
}

static int lfs_fuse_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    return lfs_write(path, buf, size, offset);
}

static int lfs_fuse_create(const char *path, mode_t mode,
                           struct fuse_file_info *fi)
{
    (void)fi;
    return lfs_create(path, mode);
}

static int lfs_fuse_mkdir(const char *path, mode_t mode)
{
    return lfs_mkdir(path, mode);
}

static int lfs_fuse_rmdir(const char *path)
{
    return lfs_rmdir(path);
}

static int lfs_fuse_unlink(const char *path)
{
    (void)path;
    /* lfs_rmdir can handle removal; for files we reuse rmdir logic
       or just return -ENOSYS if not implemented */
    return -ENOSYS;
}

static int lfs_fuse_rename(const char *from, const char *to,
                           unsigned int flags)
{
    (void)from; (void)to; (void)flags;
    return -ENOSYS;
}

/* ── FUSE operations table ──────────────────────────────────────────── */

static const struct fuse_operations lfs_oper = {
    .getattr  = lfs_fuse_getattr,
    .readdir  = lfs_fuse_readdir,
    .open     = lfs_fuse_open,
    .read     = lfs_fuse_read,
    .write    = lfs_fuse_write,
    .create   = lfs_fuse_create,
    .mkdir    = lfs_fuse_mkdir,
    .rmdir    = lfs_fuse_rmdir,
    .unlink   = lfs_fuse_unlink,
    .rename   = lfs_fuse_rename,
};

int main(int argc, char *argv[])
{
    /* default image path */
    const char *img = "logfs.img";

    /* build FUSE args: program name, [-o img=path], mountpoint, ... */
    char *fuse_argv[64];
    int   fuse_argc = 0;

    fuse_argv[fuse_argc++] = argv[0];

    /* check for -o img=... */
    int mountpoint_idx = 1;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-o", 2) == 0 && i + 1 < argc &&
            strncmp(argv[i + 1], "img=", 4) == 0) {
            img = argv[i + 1] + 4;
            i++; /* skip the -o img= arg */
            continue;
        }
        if (strncmp(argv[i], "img=", 4) == 0) {
            img = argv[i] + 4;
            continue;
        }
        fuse_argv[fuse_argc++] = argv[i];
    }

    /* mount the filesystem first */
    int r = lfs_mount(img);
    if (r) {
        fprintf(stderr, "mount.logfs: lfs_mount(%s) failed: %d\n", img, r);
        return 1;
    }
    fprintf(stderr, "mount.logfs: mounted %s\n", img);

    /* let FUSE take over */
    r = fuse_main(fuse_argc, fuse_argv, &lfs_oper, NULL);

    /* FUSE main only returns after unmount – umount is handled by
       fuse3's atexit / signal handling which calls our fusermount.
       We call lfs_umount explicitly on return. */
    lfs_umount();
    fprintf(stderr, "mount.logfs: unmounted\n");
    return r;
}
