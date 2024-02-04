#include <common/list.h>
#include <common/string.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
// #include <kernel/vma.h>
#include <sys/mman.h>
#define NVMA 15

vma vma_list[NVMA];
SpinLock vma_lock;

define_early_init(vma) {
    memset(vma_list, 0, sizeof(vma_list));
    init_spinlock(&vma_lock);
}

void copy_vma(struct proc *dst, struct proc *src) {
    for (int i = 0; i < NCVMA; i++) {
        if (src->vma[i] == NULL) {
            continue;
        }
        dst->vma[i] = src->vma[i];
        vma_dup(src->vma[i]);
        for (u64 va = src->vma[i]->start; va < src->vma[i]->end;
             va += PAGE_SIZE) {
            PTEntriesPtr pte = get_pte(&src->pgdir, va, false);
            if (pte == NULL || ((*pte & PTE_VALID) == 0)) {
                continue;
            }
            vmmap(&dst->pgdir, va, (void *)(P2K(PTE_ADDRESS(*pte))),
                  PTE_FLAGS(*pte));
        }
    }
}

vma *vma_alloc() {
    _acquire_spinlock(&vma_lock);
    for (int i = 0; i < NVMA; i++) {
        if (vma_list[i].ref == 0) {
            _release_spinlock(&vma_lock);
            return &vma_list[i];
        }
    }
    PANIC();
}

void vma_close(vma *v) {
    _acquire_spinlock(&vma_lock);
    v->ref--;
    _release_spinlock(&vma_lock);
    file_close(v->file);
}

void vma_dup(vma *vma) {
    _acquire_spinlock(&vma_lock);
    vma->ref++;
    file_dup(vma->file);
    _release_spinlock(&vma_lock);
}

void cvma_alloc(vma *vma) {

    for (int i = 0; i < NCVMA; i++) {
        auto this_proc = thisproc();
        if (this_proc->vma[i] == NULL) {
            this_proc->vma[i] = vma;
            vma_dup(vma);
            file_dup(vma->file);
            return;
        }
    }
    PANIC();
}

void writeback(vma *v, u64 addr, u64 n) {
    if ((v->permission & PTE_RO) || (v->flags & MAP_PRIVATE)) {
        return;
    }
    if ((addr % PAGE_SIZE) != 0) {
        PANIC();
    }
    File *f = v->file;
    u64 i = 0;
    u64 max = 3 * BLOCK_SIZE;
    while (i < n) {
        u64 n1 = n - i;
        if (n1 > max) {
            n1 = max;
        }
        OpContext ctx;
        bcache.begin_op(&ctx);
        inodes.lock(f->ip);
        u64 r = inodes.write(&ctx, f->ip, (u8 *)addr + i,
                             v->off + v->start - addr + i, n1);
        inodes.unlock(f->ip);
        bcache.end_op(&ctx);
        i += r;
    }
}
void uvmunmap(struct pgdir *pd, u64 va, u64 npages, int do_free) {
    u64 a;
    PTEntriesPtr pte;

    if ((va % PAGE_SIZE) != 0)
        PANIC();

    for (a = va; a < va + npages * PAGE_SIZE; a += PAGE_SIZE) {
        if ((pte = get_pte(pd, a, false)) == NULL)
            continue;
        if ((*pte & PTE_VALID) == 0)
            continue;
        if (do_free) {
            u64 ka = P2K(PTE_ADDRESS(*pte));
            kfree_page((void *)ka);
        }
        *pte = 0;
    }
}