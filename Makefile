# Makefile for Log-Structured Filesystem (Assignment 3)

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g -O2
LDFLAGS =

# libfuse3 flags (for mount.logfs)
FUSE_CFLAGS  = $(shell pkg-config --cflags fuse3 2>/dev/null || echo -I/usr/include/fuse3)
FUSE_LDFLAGS = $(shell pkg-config --libs   fuse3 2>/dev/null || echo -lfuse3)

TARGETS = mkfs.logfs test_driver mount.logfs
all: $(TARGETS)

# Library object
lfs.o: lfs.c lfs.h
	$(CC) $(CFLAGS) -c -o $@ $<

# mkfs tool
mkfs.logfs: mkfs.logfs.c lfs.o lfs.h
	$(CC) $(CFLAGS) -o $@ mkfs.logfs.c lfs.o

# Phase 1 test driver
test_driver: test_driver.c lfs.o lfs.h
	$(CC) $(CFLAGS) -o $@ test_driver.c lfs.o

# FUSE mount program
mount.logfs: mount.logfs.c lfs.o lfs.h
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ mount.logfs.c lfs.o $(FUSE_LDFLAGS)

clean:
	rm -f *.o $(TARGETS) logfs.img

.PHONY: all clean
