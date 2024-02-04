#include <common/string.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
// #define NPIPE 20
// Pipe *pipe_table[NPIPE];
// static SpinLock pipe_table_lock;
// define_early_init(pipe) {
//     init_spinlock(&pipe_table_lock);
//     for (int i = 0; i < NPIPE; i++) {
//         pipe_table[i] = NULL;
//     }
// }

static void init_pipe(Pipe *pi) {
    init_spinlock(&pi->lock);
    init_sem(&pi->wlock, 0);
    init_sem(&pi->rlock, 0);
    pi->nread = 0;
    pi->nwrite = 0;
    pi->readopen = 1;
    pi->writeopen = 1;
}
static void init_pipe_file(File *f, bool readable, bool writeable, Pipe *pipe) {
    f->type = FD_PIPE;
    f->ref = 1;
    f->readable = readable;
    f->writable = writeable;
    f->pipe = pipe;
    f->off = 0;
}
int pipeAlloc(File **f0, File **f1) {
    // TODO
    Pipe *pipe = kalloc(sizeof(Pipe));
    if (pipe == NULL) {
        return -1;
    }
    *f0 = file_alloc();
    *f1 = file_alloc();
    init_pipe_file(*f0, 1, 0, pipe);
    init_pipe_file(*f1, 0, 1, pipe);
    init_pipe(pipe);
    return 0;
}

void pipeClose(Pipe *pi, int writable) {
    // TODO
    _acquire_spinlock(&pi->lock);
    if (writable) {
        pi->writeopen = 0;
        _release_spinlock(&pi->lock);
        post_sem(&pi->rlock);
    } else {
        pi->readopen = 0;
        _release_spinlock(&pi->lock);
        post_sem(&pi->wlock);
    }

    if (!pi->readopen && !pi->writeopen) {
        _release_spinlock(&pi->lock);
        kfree(pi);
    }
}

int pipeWrite(Pipe *pi, u64 addr, int n) {
    // TODO
    _acquire_spinlock(&pi->lock);
    for (int i = 0; i < n; i++) {
        // wait
        while (pi->nwrite == pi->nread + PIPESIZE) {
            if (pi->readopen == 0 || thisproc()->killed) {
                _release_spinlock(&pi->lock);
                return -1;
            }
            // TODO: 清空写资源，写进程睡眠，然后post让读
            get_all_sem(&pi->wlock);
            post_all_sem(&pi->rlock);
            _lock_sem(&pi->wlock);
            _release_spinlock(&pi->lock);
            ASSERT(_wait_sem(&pi->wlock, false));
            _acquire_spinlock(&pi->lock);
        }
        pi->data[pi->nwrite++ % PIPESIZE] = ((char *)addr)[i];
    }
    post_all_sem(&pi->rlock);
    _release_spinlock(&pi->lock);
    return n;
}

int pipeRead(Pipe *pi, u64 addr, int n) {
    // TODO
    _acquire_spinlock(&pi->lock);
    while (pi->nread == pi->nwrite && pi->writeopen) {
        if (thisproc()->killed) {
            _release_spinlock(&pi->lock);
            return -1;
        }
        get_all_sem(&pi->rlock);
        _lock_sem(&pi->rlock);
        _release_spinlock(&pi->lock);
        ASSERT(_wait_sem(&pi->rlock, false));
        //_wait_sem(&pi->rlock, false);
        _acquire_spinlock(&pi->lock);
    }
    int i = 0;
    for (; i < n; i++) {
        if (pi->nread == pi->nwrite) {
            break;
        }
        ((char *)addr)[i] = pi->data[pi->nread++ % PIPESIZE];
    }
    post_all_sem(&pi->wlock);
    _release_spinlock(&pi->lock);
    return i;
}