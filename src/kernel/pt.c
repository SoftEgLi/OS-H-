#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/pt.h>
#define PHYSTOP 0x3f000000 /* Top physical memory */

#define MY_PAGE_COUNT (PHYSTOP / PAGE_SIZE)

extern struct page pages_ref_array[MY_PAGE_COUNT];
PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc) {
    // TODO
    // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
    // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or
    // return NULL if false. THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED
    // BY PTE.
    // if (alloc) {
    //     printk("get_pte:alloc once\n");
    // }
    if (pgdir->pt == NULL) {
        if (alloc) {
            pgdir->pt = kalloc_page();
            memset((void *)pgdir->pt, 0, PAGE_SIZE);
        } else {
            return NULL;
        }
    }
    PTEntriesPtr pgdir_pt = pgdir->pt;
    short index[4];
    for (int i = 0; i < 4; i++) {
        index[i] = (va >> (39 - i * 9)) & 0x1ff;
    }
    for (int i = 0; i < 3; i++) {
        if (!((pgdir_pt[index[i]] & PTE_TABLE) == PTE_TABLE)) {
            if (alloc) {
                for (int j = i; j < 3; j++) {
                    u64 *pt = kalloc_page();
                    memset((void *)pt, 0, PAGE_SIZE);
                    pgdir_pt[index[j]] = K2P(pt) | PTE_TABLE;
                    ASSERT(pgdir_pt[index[j]] < PHYSTOP);
                    pgdir_pt = pt;
                }

                return pgdir_pt + index[3];
            } else {
                return NULL;
            }
        }

        pgdir_pt = (PTEntriesPtr)P2K(PTE_ADDRESS(pgdir_pt[index[i]]));
    }
    // printk("*pte = %llx\n", *(pgdir_pt + (u64)index[3]));
    return pgdir_pt + index[3];
}

void init_pgdir(struct pgdir *pgdir) {
    pgdir->pt = kalloc_page();
    memset((void *)pgdir->pt, 0, PAGE_SIZE);
    init_spinlock(&pgdir->lock);
    init_list_node(&pgdir->section_head);
    init_sections(&(pgdir->section_head));
}

bool is_invalid_pte(u64 pte) {
    return (!((pte & PTE_TABLE) == PTE_TABLE)) || pte >= PHYSTOP;
}

void free_pgdir(struct pgdir *pgdir) {
    // TODO
    // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
    // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE
    free_sections(pgdir);
    PTEntriesPtr pt0 = pgdir->pt;
    if (pt0 == NULL)
        return;
    for (int i = 0; i < 512; i++) {
        if (is_invalid_pte(pt0[i]))
            continue;
        PTEntriesPtr pt1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt0[i]));
        for (int j = 0; j < 512; j++) {
            if (is_invalid_pte(pt1[j]))
                continue;
            PTEntriesPtr pt2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt1[j]));
            for (int k = 0; k < 512; k++) {
                // printk("i:%d,j:%d,k:%d\n", i, j, k);
                if (is_invalid_pte(pt2[k]))
                    continue;
                PTEntriesPtr pt3 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt2[k]));
                kfree_page(pt3);
            }
            kfree_page(pt2);
        }
        kfree_page(pt1);
    }
    kfree_page(pt0);
    pgdir->pt = NULL;
}

void attach_pgdir(struct pgdir *pgdir) {
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
}

void vmmap(struct pgdir *pd, u64 va, void *ka, u64 flags) {
    // TODO
    // Map virtual address 'va' to the physical address represented by kernel
    // address 'ka' in page directory 'pd', 'flags' is the flags for the page
    // table entry
    PTEntriesPtr pte = get_pte(pd, va, true);
    if (pte == NULL) {
        PANIC();
    }
    if (*pte & PTE_VALID) {
        kfree_page((void *)P2K(PTE_ADDRESS(*pte)));
    }
    *pte = K2P((u64)ka) | flags;
    arch_tlbi_vmalle1is();
    // printk("vmmap:ka = %llx, *pte = %llx\n", (u64)ka, *pte);
    // printk("vmmap:pages_ref_array index is %llx\n", K2P((u64)ka) /
    // PAGE_SIZE);
    _increment_rc(&pages_ref_array[K2P(ka) / PAGE_SIZE].ref);
}

/*
 * Copy len bytes from p to user address va in page table pgdir.
 * Allocate physical pages if required.
 * Useful when pgdir is not the current page table.
 */
int copyout(struct pgdir *pd, void *va, void *p, usize len) {
    // TODO
    u64 va_start = (u64)va;
    u64 va_end = (u64)va + len;
    u64 read_size = 0;
    for (u64 va = va_start; va < va_end; va = PAGE_BASE(va + PAGE_SIZE)) {
        PTEntriesPtr pte = get_pte(pd, va, false);
        if (pte == NULL || !(*pte & PTE_VALID)) {
            void *new_page = kalloc_page();
            memset(new_page, 0, PAGE_SIZE);
            vmmap(pd, va, new_page, PTE_USER_DATA | PTE_VALID | PTE_RW);
            pte = get_pte(pd, va, false);
        }
        ASSERT(pte != NULL && *pte & PTE_VALID);
        void *ka = (void *)P2K(*pte & 0xfffffffffffff000) + va % PAGE_SIZE;
        if (PAGE_BASE(va) == PAGE_BASE(va_end)) {
            // 最后一页
            memcpy(ka, p + read_size, len - read_size);
            read_size += len - read_size;
        } else {
            memcpy(ka, p + read_size, PAGE_SIZE - (u64)ka % PAGE_SIZE);
            read_size += (u64)ka % PAGE_SIZE;
        }
    }

    return 0;
}
