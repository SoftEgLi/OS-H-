#include <common/list.h>
#include <common/string.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/myqueue.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#define MaxPid 10000
#define NCPU 4
// #define DEBUG
struct proc root_proc;

void kernel_entry();
void proc_entry();

SpinLock pLock;
bool pidList[MaxPid];
SpinLock pidLock;

define_early_init(procLock) {
    init_spinlock(&pLock);
    init_spinlock(&pidLock);
    memset(pidList, 0, sizeof(pidList));
    for (int i = 0; i < NCPU; i++) {
        pidList[i] = 1;
    }
}

void set_parent_to_this(struct proc *proc) {
    // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL
    _acquire_spinlock(&pLock);
    proc->parent = thisproc();
    _insert_into_list(&thisproc()->children, &proc->ptnode);
    _release_spinlock(&pLock);
}
u64 page_ceil(u64 size);

void free_vma(struct proc *p) {
    for (int i = 0; i < NCVMA; i++) {
        if (p->vma[i] != NULL) {
            writeback(p->vma[i], p->vma[i]->start, p->vma[i]->length);
            uvmunmap(&p->pgdir, p->vma[i]->start,
                     page_ceil(p->vma[i]->length) / PAGE_SIZE, 1);
            vma_close(p->vma[i]);
        }
    }
}

NO_RETURN void exit(int code) {
    // TODO
    // 1. set the exitcode
    // 2. clean up the resources
    // 3. transfer children to the root_proc, and notify the root_proc if there
    // is zombie
    // 4. sched(ZOMBIE)
    // NOTE: be careful of concurrency
    auto this = thisproc();
    this->exitcode = code;
    _acquire_spinlock(&pLock);
    // 合并子进程
    ListNode *temp = &this->children;
    // 首先，将子进程的父亲设为root
    if (!_empty_list(&this->children)) {
        ListNode *head = temp->next;
        while (head != temp) {
            auto childProc = container_of(head, struct proc, ptnode);
            childProc->parent = &root_proc;
            head = head->next;
            _detach_from_list(&childProc->ptnode);
            _insert_into_list(&root_proc.children, &childProc->ptnode);
            if (is_zombie(childProc)) {
                post_sem(&root_proc.childexit);
            }
        }
    }
    _release_spinlock(&pLock);
    kfree_page(this->kstack);
    free_vma(this);
    free_pgdir(&this->pgdir);
    OpContext ctx;
    bcache.begin_op(&ctx);
    inodes.put(&ctx, this->cwd);
    bcache.end_op(&ctx);
    free_oftable(&this->oftable);
    post_sem(&this->parent->childexit);
    _acquire_sched_lock();

    _sched(ZOMBIE);

    PANIC(); // prevent the warning of 'no_return function returns'
}

int wait(int *exitcode) {
    // TODO
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its pid and exitcode
    // NOTE: be careful of concurrency

    auto this = thisproc();
    _acquire_spinlock(&pLock); // TODO:或许可以删掉
    if (_empty_list(&this->children)) {
        _release_spinlock(&pLock);
        return -1;
    }
    _release_spinlock(&pLock);

    bool ret = wait_sem(&this->childexit);
    if (ret == false) {
        arch_wfi();
    }
    _acquire_spinlock(&pLock); // TODO:或许可以删掉

    int pid = 0;
    ListNode *temp = &this->children;

    _for_in_list(p, &this->children) {
        if (p == &this->children) {
            continue;
        }
        auto childProc = container_of(p, struct proc, ptnode);
        if (is_zombie(childProc)) {
            temp = p;
            pid = childProc->pid;
            *exitcode = childProc->exitcode;
            _acquire_spinlock(&pidLock);
            pidList[childProc->pid] = 0;
            _release_spinlock(&pidLock);
            _detach_from_list(temp);
            _release_spinlock(&pLock);
            // 回收其它
            kfree(childProc);

            return pid;
        }
    }
    PANIC();
    return 0;
}

int kill(int pid) {
    // TODO
    // Set the killed flag of the proc to true and return 0.
    // Return -1 if the pid is invalid (proc not found).
    struct MyQueue q;
    initQueue(&q);
    enqueue(&q, &root_proc);
    struct proc *pro = NULL;
    // 第一步，将
    while (!isEmpty(&q)) {
        auto proc = dequeue(&q);

        if (proc->pid == pid && !is_unused(proc)) {
            proc->killed = true;
            pro = proc;
            break;
        }
        _for_in_list(p, &proc->children) {
            if (p == &proc->children) {
                break;
            }
            auto childProc = container_of(p, struct proc, ptnode);
            enqueue(&q, childProc);
        }
    }
    if (pro == NULL) {
        return -1;
    }
    activate_proc(pro);
    return 0;
}

int start_proc(struct proc *p, void (*entry)(u64), u64 arg) {
    // TODO
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its pid
    // NOTE: be careful of concurrency
    _acquire_spinlock(&pLock);
    if (p->parent == NULL) {
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
    }
    _release_spinlock(&pLock);

    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = (u64)arg;
    int id = p->pid;
    activate_proc(p);
    return id;
}

int getAvailPidWithLock() {
    _acquire_spinlock(&pidLock);
    int i;
    for (i = 0; i < MaxPid; i++) {
        if (pidList[i] == 0) {
            pidList[i] = 1;
            break;
        }
    }
    _release_spinlock(&pidLock);
    return i;
}

void init_proc(struct proc *p) {
    // TODO
    // setup the struct proc with kstack and pid allocated
    // NOTE: be careful of concurrency
    memset(p, 0, sizeof(struct proc));
    p->pid = getAvailPidWithLock();
    init_pgdir(&p->pgdir);
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    init_schinfo(&p->schinfo);
    p->kstack = kalloc_page();
    memset(p->kstack, 0, PAGE_SIZE);
    p->kcontext =
        (KernelContext *)((u64)p->kstack + PAGE_SIZE - 16 -
                          sizeof(KernelContext) - sizeof(UserContext));
    p->ucontext =
        (UserContext *)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(UserContext));
    p->cwd = inodes.get(ROOT_INODE_NO);
}

struct proc *create_proc() {
    struct proc *p = kalloc(sizeof(struct proc));
    init_proc(p);

    return p;
}

define_init(root_proc) {
    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);
}

/*
 * Create a new process copying p as the parent.
 * Sets up stack to return as if from system call.
 */
void trap_return();

void copy_pgdir(struct pgdir *dst, struct pgdir *src) {
    ListNode *temp = src->section_head.next;
    while (temp != (&src->section_head) /* .next->prev */) {
        auto section = container_of(temp, struct section, stnode);
        for (u64 addr = section->begin; addr < section->end;
             addr = PAGE_BASE(addr + PAGE_SIZE)) {
            PTEntriesPtr pte = get_pte(src, addr, false);
            if ((pte == NULL || !(*pte & PTE_VALID)) &&
                (section->flags != ST_HEAP)) {
                printk("section->begin = %llx\n", section->begin);
                printk("section->end = %llx\n", section->end);
                printk("addr = %llx\n", addr);
                printk("flags = %llx\n", section->flags);
                printk("pte = %llx\n", (u64)pte);
                PANIC();
            }
            vmmap(dst, addr, (void *)(P2K(PTE_ADDRESS(*pte))),
                  PTE_FLAGS(*pte) | PTE_RO);
            *pte = *pte | PTE_RO;
        }
        temp = temp->next;
    }
    arch_tlbi_vmalle1is();
}

// void copy_pgdir(struct pgdir *dst, struct pgdir *src) {

//     // 创新点：实现COW

//     ASSERT(dst != NULL && dst->pt != NULL);
//     PTEntriesPtr src_pt = src->pt;
//     PTEntriesPtr dst_pt = dst->pt;
//     if ((u64)src_pt == 0xb8737801f9403fe0 ||
//         (u64)dst_pt == 0xb8737801f9403fe0) {
//         PANIC();
//     }
//     for (int i = 0; i < 512; i++) {
//         src_pt = src->pt;
//         dst_pt = dst->pt;
//         if (src_pt[i] == NULL || (!((src_pt[i] & PTE_TABLE) == PTE_TABLE))) {
//             continue;
//         }
//         void *ka1 = kalloc_page();
//         memset(ka1, 0, PAGE_SIZE);
//         dst_pt[i] = K2P(ka1) | PTE_TABLE;
//         printk("src_pt = %llx\n", (u64)src_pt[i]);
//         dst_pt = (PTEntriesPtr)P2K(dst_pt[i] & 0xfffffffffffff000);
//         src_pt = (PTEntriesPtr)P2K(src_pt[i] & 0xfffffffffffff000);
//         if ((u64)src_pt == 0xb8737801f9403fe0 ||
//             (u64)dst_pt == 0xb8737801f9403fe0) {
//             if ((u64)src_pt == 0xb8737801f9403fe0) {
//                 printk("src\n");
//             } else {
//                 printk("dst\n");
//             }
//             PANIC();
//         }
//         // 为了复原
//         PTEntriesPtr src_temp1 = src_pt;
//         PTEntriesPtr dst_temp1 = dst_pt;
//         for (int j = 0; j < 512; j++) {
//             // 复原
//             src_pt = src_temp1;
//             dst_pt = dst_temp1;
//             //
//             if (src_pt[j] == NULL || !((src_pt[j] & PTE_TABLE) == PTE_TABLE))
//             {
//                 continue;
//             }
//             void *ka2 = kalloc_page();
//             memset(ka2, 0, PAGE_SIZE);
//             dst_pt[j] = K2P(ka2) | PTE_TABLE;
//             printk("src_pt = %llx\n", (u64)src_pt[j]);
//             dst_pt = (PTEntriesPtr)P2K(dst_pt[j] & 0xfffffffffffff000);
//             src_pt = (PTEntriesPtr)P2K(src_pt[j] & 0xfffffffffffff000);
//             if ((u64)src_pt == 0xb8737801f9403fe0 ||
//                 (u64)dst_pt == 0xb8737801f9403fe0) {
//                 if ((u64)src_pt == 0xb8737801f9403fe0) {
//                     printk("src\n");
//                 } else {
//                     printk("dst\n");
//                 }
//                 PANIC();
//             }
//             PTEntriesPtr src_temp2 = src_pt;
//             PTEntriesPtr dst_temp2 = dst_pt;
//             for (int k = 0; k < 512; k++) {
//                 // 复原
//                 src_pt = src_temp2;
//                 dst_pt = dst_temp2;
//                 if (src_pt[k] == NULL ||
//                     !((src_pt[k] & PTE_TABLE) == PTE_TABLE)) {
//                     continue;
//                 }
//                 void *ka3 = kalloc_page();
//                 memset(ka3, 0, PAGE_SIZE);
//                 dst_pt[k] = K2P(ka3) | PTE_TABLE;
//                 printk("src_pt = %llx\n", (u64)src_pt[k]);
//                 dst_pt = (PTEntriesPtr)P2K(dst_pt[k] & 0xfffffffffffff000);
//                 src_pt = (PTEntriesPtr)P2K(src_pt[k] & 0xfffffffffffff000);
//                 if ((u64)src_pt == 0xb8737801f9403fe0 ||
//                     (u64)dst_pt == 0xb8737801f9403fe0) {
//                     if ((u64)src_pt == 0xb8737801f9403fe0) {
//                         printk("src\n");
//                     } else {
//                         printk("dst\n");
//                     }

//                     PANIC();
//                 }
//                 PTEntriesPtr src_temp3 = src_pt;
//                 PTEntriesPtr dst_temp3 = dst_pt;
//                 for (int l = 0; l < 512; l++) {
//                     // 复原
//                     src_pt = src_temp3;
//                     dst_pt = dst_temp3;
//                     if (src_pt[l] == NULL || !(src_pt[l] & PTE_VALID)) {
//                         continue;
//                     }
//                     if ((u64)src_pt == 0xb8737801f9403fe0 ||
//                         (u64)dst_pt == 0xb8737801f9403fe0) {
//                         PANIC();
//                     }
//                     dst_pt[l] = src_pt[l];
//                 }
//             }
//         }
//     }
//     struct sections_info sections;
//     struct section *text, *data, *bss, *heap;
//     text = container_of(src->section_head.next, struct section, stnode);
//     data = container_of(src->section_head.next->next, struct section,
//     stnode); bss = container_of(src->section_head.next->next->next, struct
//     section,
//                        stnode);
//     heap = container_of(src->section_head.next->next->next->next,
//                         struct section, stnode);
//     sections.text_begin = text->begin;
//     sections.text_end = text->end;
//     sections.data_begin = data->begin;
//     sections.data_end = data->end;
//     sections.bss_begin = bss->begin;
//     sections.bss_end = bss->end;
//     sections.heap_begin = heap->begin;
//     sections.heap_end = heap->end;
//     set_sections(dst, sections);

//     printk("copy pgdir done\n");
// }

int fork() { /* TODO: Your code here. */
    // TODO
    // 1. create a new proc
    // 2. copy the pgdir from thisproc to the new proc(支持cow)
    // 3. copy the trapframe from thisproc to the new proc
    // 4. copy the kstack from thisproc to the new proc
    // 5. set the return value of the new proc to 0
    // 6. set the return value of thisproc to the pid of the new proc
    // 7. set the parent of the new proc to thisproc
    // 8. activate the new proc
    // NOTE: be careful of concurrency
    // auto lpc = left_page_cnt();
    // printk("fork:left_page_cnt = %lld\n", lpc);
    auto this = thisproc();
    auto newProc = create_proc();
    copy_vma(newProc, this);
    copy_pgdir(&newProc->pgdir, &this->pgdir);
    copy_sections(&newProc->pgdir, &this->pgdir);
    memcpy(newProc->ucontext, this->ucontext, sizeof(UserContext));
    for (int i = 0; i < NOFILE; i++) {
        if (this->oftable.files[i] != NULL) {
            newProc->oftable.files[i] = this->oftable.files[i];
            file_dup(newProc->oftable.files[i]);
        }
    }
    newProc->cwd = this->cwd;
    inodes.share(newProc->cwd);
    newProc->ucontext->x[0] = 0;
    set_parent_to_this(newProc);
    start_proc(newProc, trap_return, 0);
    return newProc->pid;
}