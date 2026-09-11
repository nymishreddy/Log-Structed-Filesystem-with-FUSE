/* test_driver.c  –  Phase 1 test driver (Task C.4) */

#define _GNU_SOURCE
#include "lfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int dir_entries;
static int dir_filler(void *buf, const char *name, uint64_t ino, mode_t mode)
{
    (void)buf; (void)ino; (void)mode;
    printf("    direntry: %s (ino=%lu, mode=0%o)\n",
           name, (unsigned long)ino, mode);
    dir_entries++;
    return 0;
}

static void test_create_read(void)
{
    printf("\n=== Test: create + write + read ===\n");

    int r = lfs_create("/hello.txt", 0644);
    assert(r == 0);
    printf("  lfs_create /hello.txt  => OK\n");

    const char *msg = "Hello, LogFS!";
    size_t len = strlen(msg);
    r = lfs_write("/hello.txt", msg, len, 0);
    assert(r == (int)len);
    printf("  lfs_write  => %d bytes\n", r);

    char buf[64] = {0};
    r = lfs_read("/hello.txt", buf, sizeof(buf), 0);
    assert(r == (int)len);
    assert(memcmp(buf, msg, len) == 0);
    printf("  lfs_read   => %d bytes, content=\"%s\"\n", r, buf);
    printf("  PASS\n");
}

static void test_mkdir_readdir(void)
{
    printf("\n=== Test: mkdir + readdir ===\n");

    int r = lfs_mkdir("/docs", 0755);
    assert(r == 0);
    printf("  lfs_mkdir /docs  => OK\n");

    /* readdir on / */
    printf("  readdir /:\n");
    dir_entries = 0;
    r = lfs_readdir("/", NULL, dir_filler);
    assert(r == 0);
    assert(dir_entries >= 1);
    printf("  PASS (found %d entries)\n", dir_entries);

    /* readdir on /docs (should be empty except sentinel) */
    printf("  readdir /docs:\n");
    dir_entries = 0;
    r = lfs_readdir("/docs", NULL, dir_filler);
    assert(r == 0);
    assert(dir_entries == 0);
    printf("  PASS (found %d entries, expected 0)\n", dir_entries);
}

static void test_create_in_dir(void)
{
    printf("\n=== Test: create file in subdirectory ===\n");

    int r = lfs_create("/docs/notes.txt", 0644);
    assert(r == 0);
    printf("  lfs_create /docs/notes.txt  => OK\n");

    const char *data = "Some notes here.";
    r = lfs_write("/docs/notes.txt", data, strlen(data), 0);
    assert(r == (int)strlen(data));
    printf("  lfs_write  => OK\n");

    char buf[64] = {0};
    r = lfs_read("/docs/notes.txt", buf, sizeof(buf), 0);
    assert(r == (int)strlen(data));
    assert(memcmp(buf, data, strlen(data)) == 0);
    printf("  lfs_read   => \"%s\"\n", buf);

    /* readdir /docs */
    printf("  readdir /docs:\n");
    dir_entries = 0;
    r = lfs_readdir("/docs", NULL, dir_filler);
    assert(r == 0);
    assert(dir_entries >= 1);
    printf("  PASS\n");
}

static void test_rmdir(void)
{
    printf("\n=== Test: rmdir ===\n");

    /* try to rmdir non-empty directory */
    int r = lfs_rmdir("/docs");
    printf("  lfs_rmdir /docs (non-empty) => %d (expected -ENOTEMPTY=%d)\n",
           r, -ENOTEMPTY);
    assert(r == -ENOTEMPTY);

    /* remove file first */
    /* (we don't have unlink, so just test rmdir on empty dir) */
    lfs_mkdir("/empty_dir", 0755);
    r = lfs_rmdir("/empty_dir");
    assert(r == 0);
    printf("  lfs_rmdir /empty_dir  => OK\n");

    /* verify it's gone */
    struct stat st;
    r = lfs_getattr("/empty_dir", &st);
    assert(r == -ENOENT);
    printf("  lfs_getattr /empty_dir => -ENOENT (correct)\n");
    printf("  PASS\n");
}

static void test_getattr(void)
{
    printf("\n=== Test: getattr ===\n");
    struct stat st;

    int r = lfs_getattr("/", &st);
    assert(r == 0);
    assert(S_ISDIR(st.st_mode));
    printf("  / : mode=0%o size=%ld\n", st.st_mode, (long)st.st_size);

    r = lfs_getattr("/hello.txt", &st);
    assert(r == 0);
    assert(S_ISREG(st.st_mode));
    assert(st.st_size > 0);
    printf("  /hello.txt : mode=0%o size=%ld\n",
           st.st_mode, (long)st.st_size);

    r = lfs_getattr("/nonexistent", &st);
    assert(r == -ENOENT);
    printf("  /nonexistent => -ENOENT (correct)\n");
    printf("  PASS\n");
}

static void test_overwrite(void)
{
    printf("\n=== Test: overwrite write ===\n");

    const char *v1 = "version1";
    lfs_write("/hello.txt", v1, strlen(v1), 0);

    const char *v2 = "v2-overwrite";
    lfs_write("/hello.txt", v2, strlen(v2), 0);

    char buf[64] = {0};
    int r = lfs_read("/hello.txt", buf, sizeof(buf), 0);
    assert(r == (int)strlen(v2));
    assert(memcmp(buf, v2, strlen(v2)) == 0);
    printf("  overwrite => \"%s\"  PASS\n", buf);
}

int main(int argc, char *argv[])
{
    const char *img = "logfs.img";
    if (argc > 1) img = argv[1];

    printf("Phase 1 test driver – mounting %s\n", img);
    int r = lfs_mount(img);
    if (r) {
        fprintf(stderr, "mount failed: %d\n", r);
        return 1;
    }
    printf("  mounted OK\n");

    test_mkdir_readdir();
    test_create_read();
    test_create_in_dir();
    test_getattr();
    test_overwrite();
    test_rmdir();

    printf("\n=== All tests passed ===\n");

    lfs_umount();
    printf("  umounted OK\n");
    return 0;
}
