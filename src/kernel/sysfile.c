//
// File-system system calls implementation.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

#include "syscall.h"
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/string.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <sys/mman.h>

struct iovec {
    void *iov_base; /* Starting address. */
    usize iov_len;  /* Number of bytes to transfer. */
};

// MY Code
static SpinLock sysfile_lock;

define_early_init(sysfile_lock) { init_spinlock(&sysfile_lock); }

// 创新点：oftable存放File的指针
// get the file object by fd
// return null if the fd is invalid
static struct file *fd2file(int fd) {
    // TODO
    struct proc *cur_proc = thisproc();
    struct file *f = cur_proc->oftable.files[fd];
    if (f == NULL || f->ref == 0) {
        return NULL;
    }
    return f;
}

/*
 * Allocate a file descriptor for the given file.
 * Takes over file reference from caller on success.
 */
int fdalloc(struct file *f) {
    /* TODO: Lab10 Shell */
    struct proc *proc = thisproc();
    struct oftable *table = &proc->oftable;
    int i;
    for (i = 0; i < NFILE; i++) {
        if (table->files[i] == NULL) {
            table->files[i] = f;
            return i;
        }
    }
    return -1;
}

// ioctl - control device
define_syscall(ioctl, int fd, u64 request) {
    // 0x5413 is TIOCGWINSZ (I/O Control to Get the WINdow SIZe, a magic request
    // to get the stdin terminal size) in our implementation. Just ignore it.
    ASSERT(request == 0x5413);
    (void)fd;
    return 0;
}
u64 page_ceil(u64 size);

// mmap - map files or devices into memory
define_syscall(mmap, void *addr, int length, int prot, int flags, int fd,
               int offset) {
    // TODO
    // 创新点：支持中间截断
    if ((isize)addr < 0 || length < 0 || prot < 0 || flags < 0 || fd < 0) {
        return -1;
    }
    auto p = thisproc();
    File *f = fd2file(fd);
    u64 pte_flag = PTE_USER_DATA;
    ASSERT(f->ip->valid);
    if (!(f->ip->entry.type == INODE_REGULAR)) {
        printk("mmap: not a regular file\n");
        return -1;
    }

    if (!f->readable) {
        printk("mmap: Unreadable\n");
        return -1;
    }

    if ((flags & MAP_SHARED) &&
        ((prot & PROT_WRITE) && (!f->readable || !f->writable))) {
        printk("mmap: MAP_SHARED failed\n");
        return -1;
    }
    if (f->ref == 0) {
        printk("mmap: file ref = 0\n");
        return -1;
    }
    if (!(prot & PROT_WRITE)) {
        pte_flag |= PTE_RO;
    }
    vma *v = vma_alloc();
    v->permission = pte_flag;
    v->length = length;
    v->off = offset;
    v->file = f;
    v->flags = flags;
    u64 vma_end = 0;
    int i = -1;
    bool found = false;
    for (i = 0; i < NCVMA; i++) {
        if (p->vma[i] == NULL) {
            continue;
        }
        if (p->vma[i]->end > vma_end) {
            found = true;
            vma_end = p->vma[i]->end;
        }
    }
    if (!found) {
        vma_end = VMA_START;
    }
    v->start = page_ceil(vma_end);
    v->end = v->start + length;
    cvma_alloc(v);

    addr = (void *)v->start;
    return (u64)addr;
}

// munmap - unmap files or devices into memory
define_syscall(munmap, void *addr, long int length) {
    // TODO
    // printk("addr = %p, length = %lld \n", addr, (u64)length);

    if ((i64)addr < 0 || length < 0) {
        return -1;
    }
    auto p = thisproc();
    int idx = -1;
    for (int i = 0; i < NCVMA; i++) {
        if (p->vma[i] == NULL) {
            continue;
        }
        if (p->vma[i]->start <= (u64)addr && (u64)addr < p->vma[i]->end) {
            idx = i;
            break;
        }
    }
    if (idx == -1) {
        printk("VMA not found\n");
        return -1;
    }
    if ((u64)addr + length > p->vma[idx]->end) {
        PANIC();
    }
    vma *v = p->vma[idx];
    writeback(v, (u64)addr, length);
    uvmunmap(&p->pgdir, (u64)addr, length / PAGE_SIZE, 1);
    vma_close(v);
    p->vma[idx] = NULL;
    if ((u64)addr > v->start) {
        vma *v2 = vma_alloc();
        memcpy(v2, v, sizeof(vma));
        v2->end = (u64)addr;
        v2->length = (u64)addr - v->start;
        v2->ref = 0;
        v2->off = v->off;
        cvma_alloc(v2);
    }
    if ((u64)addr + length < v->end) {
        vma *v2 = vma_alloc();
        memcpy(v2, v, sizeof(vma));
        v2->start = (u64)addr + length;
        v2->length = v->end - (u64)addr - length;
        v2->ref = 0;
        v2->off = v->off + length;
        cvma_alloc(v2);
    }

    return 0;
}

// dup - duplicate a file descriptor
define_syscall(dup, int fd) {
    struct file *f = fd2file(fd);
    if (!f) {
        // MY TODO:删掉PANIC
        return -1;
    }
    fd = fdalloc(f);
    if (fd < 0)
        return -1;
    file_dup(f);
    return fd;
}

// read - read from a file descriptor
define_syscall(read, int fd, char *buffer, int size) {
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_writeable(buffer, size))
        return -1;
    return file_read(f, buffer, size);
}

// write - write to a file descriptor
define_syscall(write, int fd, char *buffer, int size) {
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_readable(buffer, size))
        return -1;
    return file_write(f, buffer, size);
}

// writev - write data into multiple buffers
define_syscall(writev, int fd, struct iovec *iov, int iovcnt) {
    struct file *f = fd2file(fd);
    struct iovec *p;
    if (!f || iovcnt <= 0 || !user_readable(iov, sizeof(struct iovec) * iovcnt))
        return -1;
    usize tot = 0;
    for (p = iov; p < iov + iovcnt; p++) {
        if (!user_readable(p->iov_base, p->iov_len))
            return -1;
        tot += file_write(f, p->iov_base, p->iov_len);
    }
    return tot;
}

// close - close a file descriptor
define_syscall(close, int fd) {
    /* TODO: LabFinal */
    // TODO:考虑pipe
    struct oftable *table = &thisproc()->oftable;
    struct file *f = table->files[fd];
    if (f == NULL) {
        return -1;
    }
    table->files[fd] = NULL;
    file_close(f);
    return 0;
}

// fstat - get file status
define_syscall(fstat, int fd, struct stat *st) {
    struct file *f = fd2file(fd);
    if (!f || !user_writeable(st, sizeof(*st)))
        return -1;
    return file_stat(f, st);
}

// newfstatat - get file status (on some platform also called fstatat64, i.e. a
// 64-bit version of fstatat)
define_syscall(newfstatat, int dirfd, const char *path, struct stat *st,
               int flags) {
    if (!user_strlen(path, 256) || !user_writeable(st, sizeof(*st)))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_fstatat: dirfd unimplemented\n");
        return -1;
    }
    if (flags != 0) {
        printk("sys_fstatat: flags unimplemented\n");
        return -1;
    }

    Inode *ip;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = namei(path, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.lock(ip);
    stati(ip, st);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);

    return 0;
}

// is the directory `dp` empty except for "." and ".." ?
static int isdirempty(Inode *dp) {
    usize off;
    DirEntry de;

    for (off = 2 * sizeof(de); off < dp->entry.num_bytes; off += sizeof(de)) {
        if (inodes.read(dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
            PANIC();
        if (de.inode_no != 0)
            return 0;
    }
    return 1;
}

// unlinkat - delete a name and possibly the file it refers to
define_syscall(unlinkat, int fd, const char *path, int flag) {
    ASSERT(fd == AT_FDCWD && flag == 0);
    Inode *ip, *dp;
    DirEntry de;
    char name[FILE_NAME_MAX_LENGTH];
    usize off;
    if (!user_strlen(path, 256))
        return -1;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((dp = nameiparent(path, name, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.lock(dp);

    // Cannot unlink "." or "..".
    if (strncmp(name, ".", FILE_NAME_MAX_LENGTH) == 0 ||
        strncmp(name, "..", FILE_NAME_MAX_LENGTH) == 0)
        goto bad;

    usize inumber = inodes.lookup(dp, name, &off);
    if (inumber == 0)
        goto bad;
    ip = inodes.get(inumber);
    inodes.lock(ip);

    if (ip->entry.num_links < 1)
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY && !isdirempty(ip)) {
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        goto bad;
    }

    memset(&de, 0, sizeof(de));
    if (inodes.write(&ctx, dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY) {
        dp->entry.num_links--;
        inodes.sync(&ctx, dp, true);
    }
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    ip->entry.num_links--;
    inodes.sync(&ctx, ip, true);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;

bad:
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    bcache.end_op(&ctx);
    return -1;
}

/**
    @brief create an inode at `path` with `type`.

    If the inode exists, just return it.

    If `type` is directory, you should also create "." and ".." entries and link
   them with the new inode.

    @note BE careful of handling error! You should clean up ALL the resources
   you allocated and free ALL acquired locks when error occurs. e.g. if you
   allocate a new inode "/my/dir", but failed to create ".", you should free the
   inode "/my/dir" before return.

    @see `nameiparent` will find the parent directory of `path`.

    @return Inode* the created inode, or NULL if failed.
 */
Inode *create(const char *path, short type, short major, short minor,
              OpContext *ctx) {
    /* TODO: LabFinal */
    _acquire_spinlock(&sysfile_lock);
    Inode *child = namei(path, ctx);
    if (child != NULL) {
        _release_spinlock(&sysfile_lock);
        return child;
    }
    char create_name[FILE_NAME_MAX_LENGTH] = {0};
    Inode *parent = nameiparent(path, create_name, ctx);
    if (parent == NULL) {
        _release_spinlock(&sysfile_lock);
        return NULL;
    }
    usize inode_no = inodes.alloc(ctx, type);
    child = inodes.get(inode_no);
    inodes.lock(child);
    inodes.lock(parent);

    child->entry.major = major;
    child->entry.minor = minor;
    child->entry.type = type;
    child->entry.num_links = 1;
    ASSERT(type == INODE_REGULAR || type == INODE_DIRECTORY ||
           type == INODE_DEVICE);

    if (type == INODE_DIRECTORY) {
        if (inodes.insert(ctx, child, ".", inode_no) == (usize)-1 ||
            inodes.insert(ctx, child, "..", parent->inode_no) == (usize)-1) {
            inodes.unlock(child);
            inodes.unlock(parent);
            inodes.put(ctx, parent);
            inodes.put(ctx, child);
            _release_spinlock(&sysfile_lock);
            return NULL;
        }
    }

    inodes.sync(ctx, child, true);
    inodes.unlock(child);
    if (inodes.insert(ctx, parent, create_name, inode_no) == (usize)-1) {
        inodes.unlock(child);
        inodes.unlock(parent);
        inodes.put(ctx, parent);
        inodes.put(ctx, child);
        _release_spinlock(&sysfile_lock);
        return NULL;
    }
    inodes.sync(ctx, parent, true);
    inodes.unlock(parent);
    inodes.put(ctx, parent);
    _release_spinlock(&sysfile_lock);

    return child;
}

// openat - open a file
define_syscall(openat, int dirfd, const char *path, int omode) {
    int fd;
    struct file *f;
    Inode *ip;
    // printk("openat once\n");
    if (!user_strlen(path, 256))
        return -1;

    if (dirfd != AT_FDCWD) {
        printk("sys_openat: dirfd unimplemented\n");
        return -1;
    }

    OpContext ctx;
    bcache.begin_op(&ctx);
    if (omode & O_CREAT) {
        // FIXME: Support acl mode.
        ip = create(path, INODE_REGULAR, 0, 0, &ctx);
        if (ip == 0) {
            bcache.end_op(&ctx);
            PANIC();

            return -1;
        }
    } else {
        // printk("openat:path = %s\n", path);
        if ((ip = namei(path, &ctx)) == 0) {
            bcache.end_op(&ctx);
            return -1;
        }

        inodes.lock(ip);
    }
    if ((f = file_alloc()) == 0 || (fd = fdalloc(f)) < 0) {
        if (f)
            file_close(f);
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    bcache.end_op(&ctx);

    f->type = FD_INODE;
    f->ip = ip;
    f->off = 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    return fd;
}

// mkdirat - create a directory
define_syscall(mkdirat, int dirfd, const char *path, int mode) {
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mkdirat: dirfd unimplemented\n");
        return -1;
    }
    if (mode != 0) {
        printk("sys_mkdirat: mode unimplemented\n");
        return -1;
    }
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DIRECTORY, 0, 0, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

// mknodat - create a special or ordinary file
define_syscall(mknodat, int dirfd, const char *path, mode_t mode, dev_t dev) {
    printk("mknodat:mode = %d\n", mode);
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mknodat: dirfd unimplemented\n");
        return -1;
    }

    unsigned int ma = major(dev);
    unsigned int mi = minor(dev);
    printk("mknodat: path '%s', major:minor %u:%u\n", path, ma, mi);
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DEVICE, (short)ma, (short)mi, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    // printk("mknodat:inode_no = %lld\n", ip->inode_no);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

// chdir - change current working directory
define_syscall(chdir, const char *path) {
    // TODO
    // change the cwd (current working dictionary) of current process to 'path'
    // you may need to do some validations
    // TODO 当前假设为绝对路径
    if (strlen(path) > FILE_NAME_MAX_LENGTH) {
        return -1;
    }
    if (path[0] == ' ') {
        return -1;
    }

    OpContext ctx;
    struct proc *cur_proc = thisproc();
    bcache.begin_op(&ctx);
    Inode *pre_cwd = cur_proc->cwd;
    Inode *cwd = namei(path, &ctx);
    if (cwd == NULL) {
        bcache.end_op(&ctx);
        return -1;
    }
    cur_proc->cwd = cwd;
    inodes.put(&ctx, pre_cwd);
    bcache.end_op(&ctx);
    return 0;
}

// pipe2 - create a pipe
define_syscall(pipe2, int pipefd[2], int flags) {
    // TODO
    // you can ignore the flags here,
    // or if you like, do some assertions to filter out unimplemented flags
    File *f_read = NULL;
    File *f_write = NULL;
    if (flags) {
    }
    if (pipeAlloc(&f_read, &f_write)) {
        return -1;
    }
    int fd_read = -1;
    int fd_write = -1;
    if ((fd_read = fdalloc(f_read)) < 0 || (fd_write = fdalloc(f_write)) < 0) {
        if (fd_read >= 0) {
            thisproc()->oftable.files[fd_read] = NULL;
            file_close(f_read);
        }
        if (fd_write >= 0) {
            thisproc()->oftable.files[fd_write] = NULL;
            file_close(f_write);
        }
        return -1;
    }
    pipefd[0] = fd_read;
    pipefd[1] = fd_write;
    return 0;
}