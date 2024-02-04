#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <fs/file.h>
#include <fs/inode.h>
#include <kernel/pt.h>
#include <kernel/schinfo.h>
#define NCVMA 5
enum procstate { UNUSED, RUNNABLE, RUNNING, SLEEPING, DEEPSLEEPING, ZOMBIE };

typedef struct UserContext {
    // TODO: customize your trap frame
    __uint128_t q0;
    u64 tpidr0, spsr, elr, sp_el0;
    u64 x[31]; // x0-x8,x9-x18
    u64 padding;
} UserContext;

typedef struct KernelContext {
    // TODO: customize your context
    u64 lr, x0, x1;
    // x2-x29
    u64 x[28];
    u64 padding_zero;
} KernelContext;

typedef struct vma {
    u64 start;
    u64 end;
    long int length;
    u64 off;
    u64 permission;
    u64 flags;
    File *file;
    struct vma *next;
    int ref;
} vma;

struct proc {
    bool killed;
    bool idle;
    int pid;
    int exitcode;
    enum procstate state;
    Semaphore childexit;
    ListNode children;
    ListNode ptnode;
    struct proc *parent;
    struct schinfo schinfo;
    struct pgdir pgdir;
    void *kstack;
    UserContext *ucontext;
    KernelContext *kcontext;
    struct oftable oftable;
    Inode *cwd; // current working dictionary
    vma *vma[NCVMA];
};

// void init_proc(struct proc*);
WARN_RESULT struct proc *create_proc();
int start_proc(struct proc *, void (*entry)(u64), u64 arg);
NO_RETURN void exit(int code);
WARN_RESULT int wait(int *exitcode);
WARN_RESULT int kill(int pid);
WARN_RESULT int fork();
vma *vma_alloc();
void writeback(vma *v, u64 addr, u64 n);
void uvmunmap(struct pgdir *pd, u64 va, u64 npages, int do_free);
void copy_vma(struct proc *dst, struct proc *src);
void cvma_alloc(vma *vma);
void vma_close(vma *v);
void vma_dup(vma *v);