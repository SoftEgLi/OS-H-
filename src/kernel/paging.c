#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/string.h>
#include <fs/block_device.h>
#include <fs/cache.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>

#define STACK_TOP 0x800000
define_rest_init(paging) {
    // TODO
}
static bool is_valid_pte(PTEntriesPtr pte) {
    return (pte != NULL) || (!((*pte) & (u64)PTE_VALID));
}
static void init_section(struct section *sec) { init_list_node(&sec->stnode); }

void init_sections(ListNode *section_head) {
    // TODO
    struct section *text, *data, *bss, *heap, *stack;
    text = kalloc(sizeof(struct section));
    data = kalloc(sizeof(struct section));
    bss = kalloc(sizeof(struct section));
    heap = kalloc(sizeof(struct section));
    stack = kalloc(sizeof(struct section));

    init_section(text);
    init_section(data);
    init_section(bss);
    init_section(heap);
    init_section(stack);

    text->flags = ST_TEXT;
    data->flags = ST_DATA;
    bss->flags = ST_BSS;
    heap->flags = ST_HEAP;
    stack->flags = ST_STACK;

    text->begin = 0;
    text->end = PAGE_SIZE;
    data->begin = PAGE_SIZE;
    data->end = PAGE_SIZE * 2;
    bss->begin = PAGE_SIZE * 2;
    bss->end = PAGE_SIZE * 3;
    heap->begin = PAGE_SIZE * 3;
    heap->end = PAGE_SIZE * 4;
    stack->end = STACK_TOP;
    stack->begin = heap->end;

    _insert_into_list(section_head, &stack->stnode);
    _insert_into_list(section_head, &heap->stnode);
    _insert_into_list(section_head, &bss->stnode);
    _insert_into_list(section_head, &data->stnode);
    _insert_into_list(section_head, &text->stnode);
}

void copy_sections(struct pgdir *dst, struct pgdir *src) {
    struct sections_info sections;
    get_sections(src, &sections);
    set_sections(dst, sections);
}

struct section *get_section_by_va(u64 va) {
    // TODO
    struct proc *p = thisproc();
    struct pgdir *pd = &p->pgdir;
    _acquire_spinlock(&pd->lock);
    ListNode *head = pd->section_head.next;
    while (head != &pd->section_head) {
        struct section *sec = container_of(head, struct section, stnode);
        if (va >= sec->begin && va < sec->end) {
            _release_spinlock(&pd->lock);
            return sec;
        }
        head = head->next;
    }
    _release_spinlock(&pd->lock);
    return NULL;
}

void get_sections(struct pgdir *pd, struct sections_info *secs) {
    // MY func
    _acquire_spinlock(&pd->lock);
    struct section *text, *data, *bss, *heap, *stack;
    text = container_of(pd->section_head.next, struct section, stnode);
    data = container_of(pd->section_head.next->next, struct section, stnode);
    bss =
        container_of(pd->section_head.next->next->next, struct section, stnode);
    heap = container_of(pd->section_head.next->next->next->next, struct section,
                        stnode);
    stack = container_of(pd->section_head.next->next->next->next->next,
                         struct section, stnode);
    secs->text_begin = text->begin;
    secs->text_end = text->end;
    secs->data_begin = data->begin;
    secs->data_end = data->end;
    secs->bss_begin = bss->begin;
    secs->bss_end = bss->end;
    secs->heap_begin = heap->begin;
    secs->heap_end = heap->end;
    secs->stack_begin = stack->begin;
    secs->stack_end = stack->end;
    _release_spinlock(&pd->lock);
}
void set_sections(struct pgdir *pd, struct sections_info secs) {
    // MY func
    _acquire_spinlock(&pd->lock);
    struct section *text, *data, *bss, *heap, *stack;
    text = container_of(pd->section_head.next, struct section, stnode);
    data = container_of(pd->section_head.next->next, struct section, stnode);
    bss =
        container_of(pd->section_head.next->next->next, struct section, stnode);
    heap = container_of(pd->section_head.next->next->next->next, struct section,
                        stnode);
    stack = container_of(pd->section_head.next->next->next->next->next,
                         struct section, stnode);
    text->begin = secs.text_begin;
    text->end = secs.text_end;
    data->begin = secs.data_begin;
    data->end = secs.data_end;
    bss->begin = secs.bss_begin;
    bss->end = secs.bss_end;
    heap->begin = secs.heap_begin;
    heap->end = secs.heap_end;
    stack->begin = secs.stack_begin;
    stack->end = secs.stack_end;
    _release_spinlock(&pd->lock);
}

void free_sections(struct pgdir *pd) {
    // TODO
    struct section *sec;
    _acquire_spinlock(&pd->lock);
    ListNode *head = &pd->section_head;
    while (!_empty_list(head)) {
        auto node = _detach_from_list(head);
        sec = container_of(node, struct section, stnode);
        for (u64 va = sec->begin; va < sec->end;
             va = PAGE_BASE(va + PAGE_SIZE)) {
            PTEntriesPtr pte = get_pte(pd, va, false);
            if ((pte != NULL) && (*pte & PTE_VALID)) {
                kfree_page((void *)P2K(PTE_ADDRESS(*pte)));
                *pte = 0;
            }
        }
        kfree(sec);
    }
    PTEntriesPtr pte = get_pte(pd, STACK_TOP, false);
    if (pte != NULL && (*pte & PTE_VALID)) {
        kfree_page((void *)P2K(PTE_ADDRESS(*pte)));
        *pte = 0;
    }
    _release_spinlock(&pd->lock);
}

static void recycle_sec_page(struct section *sec, u64 pre_end) {
    struct proc *p = thisproc();
    u64 cur_end = sec->end;
    u64 begin_pd_addr = PAGE_BASE((cur_end + PAGE_SIZE - 1));
    u64 end_pd_addr = PAGE_BASE((pre_end - 1));
    for (u64 i = begin_pd_addr; i <= end_pd_addr; i += PAGE_SIZE) {
        PTEntriesPtr pte = get_pte(&p->pgdir, i, FALSE);
        if (pte == NULL) {
            continue;
        }
        if (*pte & PTE_VALID) {
            kfree_page((void *)P2K(PTE_ADDRESS((*pte))));
            (*pte) = 0;
        }
        // 释放掉页表项
    }
}

u64 sbrk(i64 size) {
    // TODO:
    // Increase the heap size of current process by `size`
    // If `size` is negative, decrease heap size
    // `size` must be a multiple of PAGE_SIZE
    // Return the previous heap_end
    ASSERT(size % PAGE_SIZE == 0);
    struct proc *p = thisproc();
    struct pgdir *pd = &(p->pgdir);
    _acquire_spinlock(&pd->lock);
    struct section *heap = container_of(pd->section_head.next->next->next->next,
                                        struct section, stnode);
    u64 heap_end = heap->end;
    if (size >= 0) {
        heap->end += size;
    } else {
        heap->end += size;
        if (heap->end < heap->begin) {
            _release_spinlock(&pd->lock);
            return -1;
        }
        // printk("pre_end = %lld,head->end = %lld\n", pre_end, heap->end);
        recycle_sec_page(heap, heap_end);
        arch_tlbi_vmalle1is();
    }
    _release_spinlock(&pd->lock);
    return heap_end;
}

int mmap_handler(u64 va, u64 iss) {
    // printk("mmap_handler:va = %llx\n", va);
    auto p = thisproc();
    vma *v = NULL;
    for (int i = 0; i < NCVMA; i++) {
        if (p->vma[i] == NULL) {
            continue;
        }
        if (p->vma[i]->start <= va && p->vma[i]->end > va) {
            v = p->vma[i];
            break;
        }
    }
    if (v == NULL) {
        return -1;
    }
    if (iss == 13)
        return -2;
    if ((iss == 15) && (v->permission & PTE_RO))
        return -3;

    va = PAGE_BASE(va);
    void *mem = kalloc_page();
    if (mem == NULL)
        return -4;
    memset(mem, 0, PAGE_SIZE);
    vmmap(&p->pgdir, va, mem, v->permission);
    File *f = v->file;
    inodes.lock(f->ip);
    inodes.read(f->ip, mem, v->off + va - v->start, PAGE_SIZE);
    inodes.unlock(f->ip);
    return 0;
}

int pgfault_handler(u64 iss) {
    struct proc *p = thisproc();
    struct pgdir *pd = &p->pgdir;
    u64 addr = arch_get_far(); // Attempting to access this address caused the
                               // page fault
    // vma:
    if (mmap_handler(addr, iss) == 0) {
        return 0;
    }
    // TODO:
    // 1. Find the section struct that contains the faulting address `addr`
    if (iss) {
    }
    // printk("pagfault_handler: addr = %llx\n", addr);
    struct section *sec = NULL;
    _acquire_spinlock(&pd->lock);
    ListNode *head = pd->section_head.next;
    while (head != &pd->section_head) {
        struct section *temp_sec = container_of(head, struct section, stnode);
        if (addr >= temp_sec->begin && addr < temp_sec->end) {
            sec = temp_sec;
            break;
        }
        head = head->next;
    }
    // 2. Check section flags to determine page fault type
    // 3. Handle the page fault accordingly
    if (sec == NULL) {
        // MYTODO 改成kill
        if (kill(p->pid)) {
        }
        // PANIC();
        _release_spinlock(&pd->lock);
        return 0;
    }
    if (sec->flags == (u64)ST_HEAP) {
        // printk("Heap\n");
        void *new_page = kalloc_page();
        PTEntriesPtr pte = get_pte(pd, addr, false);
        if (pte != NULL) {
            printk("pte = %llx\n", (u64)*pte);
        }
        if (pte == NULL || !(*pte & PTE_VALID)) {
            // Lazy Allocation
            printk("Heap:Lazy Allocation\n");
            pte = get_pte(pd, addr, true);
            vmmap(pd, addr, new_page, PTE_RW | PTE_VALID | PTE_USER_DATA);
        } else if ((u64)*pte & (u64)PTE_RO) {
            // COW
            printk("Heap:COW\n");
            void *old_page = (void *)P2K(PTE_ADDRESS(*pte));
            memcpy(new_page, old_page, PAGE_SIZE);
            vmmap(pd, addr, new_page, PTE_RW | PTE_VALID | PTE_USER_DATA);
        } else {
            // MYTODO 改成PANIC
            if (kill(p->pid)) {
            }
            // PANIC();
        }
        arch_tlbi_vmalle1is();
    } else if (sec->flags == (u64)ST_BSS) {
        // printk("Bss\n");
        PTEntriesPtr pte = get_pte(pd, addr, false);
        if (pte == NULL || !(*pte & PTE_VALID)) {
            // MYTODO:PANIC改成kill

            if (kill(p->pid)) {
            }
            // PANIC();
        }
        if (*pte & PTE_RO) {
            // bss段的COW
            // printk("Bss:COW\n");
            void *new_page = kalloc_page();
            void *old_page = (void *)P2K(PTE_ADDRESS(*pte));
            memcpy(new_page, old_page, PAGE_SIZE);
            vmmap(pd, addr, new_page,
                  PTE_RW | PTE_VALID | PTE_USER_DATA | PTE_BSS);
        }
        arch_tlbi_vmalle1is();
    } else if (sec->flags == (u64)ST_DATA) {
        // printk("Data\n");
        PTEntriesPtr pte = get_pte(pd, addr, false);
        if (pte == NULL || !(*pte & PTE_VALID)) {
            PANIC();
        }
        if ((*pte & PTE_RO)) {
            // COW
            void *new_page = kalloc_page();
            void *old_page = (void *)P2K(PTE_ADDRESS(*pte));
            memcpy(new_page, old_page, PAGE_SIZE);
            vmmap(pd, addr, new_page, PTE_RW | PTE_VALID | PTE_USER_DATA);
        }
        arch_tlbi_vmalle1is();

        // MYTODO:改成kill
    } else if (sec->flags == (u64)ST_TEXT) {
        // printk("text\n");
        // PANIC();
        if (kill(p->pid)) {
        }
        // MYTODO:改成kill
    } else if (sec->flags == (u64)ST_STACK) {
        // printk("stack\n");
        PTEntriesPtr pte = get_pte(pd, addr, false);
        if (!is_valid_pte(pte)) {
            // MYTODO 改成kill
            if (kill(p->pid)) {
            }
            // PANIC();
        }
        if (*pte & PTE_RO) {
            // COW
            void *new_page = kalloc_page();
            void *old_page = (void *)P2K(PTE_ADDRESS(*pte));
            memcpy(new_page, old_page, PAGE_SIZE);
            vmmap(pd, addr, new_page, PTE_RW | PTE_VALID | PTE_USER_DATA);
        } else {
            // Lazy Allocation
            PANIC();
            // if (kill(p->pid)) {
            // }
        }
        // stack
    }
    _release_spinlock(&pd->lock);

    // 4. Return to user code or kill the process
    // p->ucontext->elr = p->ucontext->elr - 4;

    return 0;
}