#include "file.h"
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/spinlock.h>
#include <fs/inode.h>
#include <kernel/mem.h>

// My code
#include <fs/pipe.h>

// the global file table.
static struct ftable ftable;

void init_ftable() {
    // TODO: initialize your ftable.
    ftable.file_count = 0;
    init_spinlock(&ftable.lock);
    for (int i = 0; i < NFILE; i++) {
        ftable.files[i].ref = 0;
    }
}

void init_file(File *f) {
    f->ip = NULL;
    f->off = 0;
    f->pipe = NULL;
    f->readable = false;
    f->writable = false;
    f->ref = 1;
    f->type = FD_NONE;
}

void init_oftable(struct oftable *oftable) {
    // TODO: initialize your oftable for a new process.
    for (int i = 0; i < NFILE; i++) {
        oftable->files[i] = NULL;
    }
}

void free_oftable(struct oftable *oftable) {
    for (int i = 0; i < NOFILE; i++) {
        if (oftable->files[i] != NULL) {
            file_close(oftable->files[i]);
        }
    }
}

/* Allocate a file structure. */
struct file *file_alloc() {
    /* TODO: LabFinal */
    _acquire_spinlock(&ftable.lock);
    // 创新点：通过file_count来判断是否需要遍历。
    if (ftable.file_count < NFILE) {
        ftable.file_count++;
        init_file(&ftable.files[ftable.file_count - 1]);
        _release_spinlock(&ftable.lock);
        return &ftable.files[ftable.file_count - 1];
    }
    for (int i = 0; i < NFILE; i++) {
        if (ftable.files[i].ref == 0) {
            init_file(&ftable.files[i]);
            _release_spinlock(&ftable.lock);
            return &ftable.files[i];
        }
    }
    PANIC();
    return 0;
}

/* Increment ref count for file f. */
struct file *file_dup(struct file *f) {
    /* TODO: LabFinal */
    _acquire_spinlock(&ftable.lock);
    f->ref++;
    _release_spinlock(&ftable.lock);
    return f;
}

/* Close file f. (Decrement ref count, close when reaches 0.) */
void file_close(struct file *f) { /* TODO: LabFinal */
    _acquire_spinlock(&ftable.lock);
    f->ref--;
    if (f->ref == 0) {
        _release_spinlock(&ftable.lock); // begin_op may sleep
        ASSERT(f->ref == 0);
        OpContext cpx;
        bcache.begin_op(&cpx);
        if (f->type == FD_INODE) {
            inodes.put(&cpx, f->ip);
        } else if (f->type == FD_PIPE) {
            pipeClose(f->pipe, f->writable);
        } else {
            PANIC();
        }
        bcache.end_op(&cpx);
        ASSERT(f->ref == 0);
        return;
    }
    _release_spinlock(&ftable.lock);
}

/* Get metadata about file f. */
int file_stat(struct file *f, struct stat *st) {
    /* TODO: LabFinal */
    _acquire_spinlock(&ftable.lock);
    ASSERT(f->type == FD_INODE);
    if (f->ip == NULL) {
        _release_spinlock(&ftable.lock);
        return -1;
    }
    _release_spinlock(&ftable.lock);
    inodes.lock(f->ip);
    stati(f->ip, st);
    inodes.unlock(f->ip);
    return 0;
}

/* Read from file f. */
isize file_read(struct file *f, char *addr, isize n) {
    /* TODO: LabFinal */
    _acquire_spinlock(&ftable.lock);
    if (f->readable == 0) {
        return -1;
    }
    _release_spinlock(&ftable.lock);
    isize result = 0;
    if (f->type == FD_PIPE) {
        result = pipeRead(f->pipe, (u64)addr, n);
    } else {
        inodes.lock(f->ip);
        result = inodes.read(f->ip, (u8 *)addr, f->off, n);
        inodes.unlock(f->ip);
        f->off += result;
    }
    return result;
}

/* Write to file f. */
isize file_write(struct file *f, char *addr, isize n) {
    /* TODO: LabFinal */
    _acquire_spinlock(&ftable.lock);
    OpContext ctx;
    if (f->writable == 0) {
        return -1;
    }
    _release_spinlock(&ftable.lock);
    bcache.begin_op(&ctx);
    isize result = 0;
    if (f->type == FD_PIPE) {
        result = pipeWrite(f->pipe, (u64)addr, n);
    } else {
        inodes.lock(f->ip);
        result = inodes.write(&ctx, f->ip, (u8 *)addr, f->off, n);
        inodes.sync(&ctx, f->ip, true);
        inodes.unlock(f->ip);
        f->off += result;
    }
    bcache.end_op(&ctx);
    return result;
}