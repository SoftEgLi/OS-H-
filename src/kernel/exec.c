#include <aarch64/trap.h>
#include <common/defines.h>
#include <common/string.h>
#include <elf.h>
#include <fs/file.h>
#include <fs/inode.h>
#include <kernel/console.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
// My COde
#include <kernel/printk.h>

#define MAX_PNUM 10
#define MAX_ARGC 128
// static u64 auxv[][2] = {{AT_PAGESZ, PAGE_SIZE}};
extern int fdalloc(struct file *f);

u64 eight_ceil(u64 size) { return ((size - 1) / 8 + 1) * 8; }
u64 page_ceil(u64 size) { return ((size - 1) / PAGE_SIZE + 1) * PAGE_SIZE; }

void load_into_memory(struct pgdir *pgdir, Inode *ip, struct sections_info secs,
                      Elf64_Phdr p_header) {
    u64 file_start_va = p_header.p_vaddr;
    u64 file_end_va = p_header.p_vaddr + p_header.p_filesz;
    for (u64 addr = file_start_va; addr < file_end_va;
         addr = PAGE_BASE(addr + PAGE_SIZE)) {
        void *ka = kalloc_page();
        memset(ka, 0, PAGE_SIZE);
        if (PAGE_BASE(file_end_va) == PAGE_BASE(addr)) {
            // 同一页
            inodes.lock(ip);
            inodes.read(ip, (u8 *)ka + addr % PAGE_SIZE,
                        p_header.p_offset + addr - file_start_va,
                        file_end_va - addr);
            inodes.unlock(ip);
        } else {
            inodes.lock(ip);
            inodes.read(ip, (u8 *)ka + addr % PAGE_SIZE,
                        p_header.p_offset + addr - file_start_va,
                        PAGE_SIZE - (addr % PAGE_SIZE));
            inodes.unlock(ip);
        }

        if (addr >= secs.text_begin && addr < secs.text_end) {
            vmmap(pgdir, addr, ka, PTE_VALID | PTE_USER_DATA | PTE_RO);
        } else if (addr >= secs.data_begin && addr < secs.data_end) {
            vmmap(pgdir, addr, ka, PTE_VALID | PTE_USER_DATA | PTE_RW);
        } else {
            PANIC();
            vmmap(pgdir, addr, ka, PTE_VALID | PTE_USER_DATA | PTE_RW);
        }
    }
    for (u64 addr = page_ceil(file_end_va);
         addr < p_header.p_vaddr + p_header.p_memsz;
         addr = PAGE_BASE(addr + PAGE_SIZE)) {
        vmmap(pgdir, addr, get_zero_page(),
              PTE_VALID | PTE_USER_DATA | PTE_RO | PTE_BSS);
    }
}
int execve(const char *path, char *const argv[], char *const envp[]) {
    // printk("In execve\n");
    // 创新点：实现环境变量的拷贝
    // if (envp) {
    // }
    OpContext cpx;
    bcache.begin_op(&cpx);
    Inode *ip = namei(path, &cpx);
    bcache.end_op(&cpx);
    if (ip == NULL) {
        printk("No such file\n");
        return -1;
    }
    struct pgdir new_pgdir;
    init_pgdir(&new_pgdir);
    Elf64_Ehdr elf_header;
    inodes.lock(ip);
    inodes.read(ip, (u8 *)&elf_header, 0, sizeof(Elf64_Ehdr));
    inodes.unlock(ip);
    if (!(elf_header.e_ident[EI_MAG0] == 0x7f) ||
        !(elf_header.e_ident[EI_MAG1] == 0x45) ||
        !(elf_header.e_ident[EI_MAG2] == 0x4c) ||
        !(elf_header.e_ident[EI_MAG3] == 0x46))
        return -1;

    Elf64_Phdr p_header[MAX_PNUM];
    ASSERT(elf_header.e_phnum <= MAX_PNUM);
    inodes.lock(ip);
    inodes.read(ip, (u8 *)p_header, elf_header.e_phoff,
                sizeof(Elf64_Phdr) * elf_header.e_phnum);
    inodes.unlock(ip);
    struct sections_info secs;

    for (int i = 0; i < elf_header.e_phnum; i++) {
        if (p_header[i].p_type == (u32)PT_LOAD) {
            if ((p_header[i].p_flags & PF_W) && (p_header[i].p_flags & PF_R)) {

                secs.data_begin = p_header[i].p_vaddr;
                secs.data_end = p_header[i].p_vaddr + p_header[i].p_filesz;
                secs.bss_begin = p_header[i].p_vaddr + p_header[i].p_filesz;
                secs.bss_end = p_header[i].p_vaddr + p_header[i].p_memsz;
                secs.heap_begin = p_header[i].p_vaddr + p_header[i].p_memsz;
                secs.heap_end = secs.heap_begin + PAGE_SIZE * 5;
            } else if ((p_header[i].p_flags & PF_X) &&
                       (p_header[i].p_flags & PF_R)) {
                secs.text_begin = p_header[i].p_vaddr;
                secs.text_end = p_header[i].p_vaddr + p_header[i].p_filesz;
            }
        }
    }
    secs.stack_begin = STACK_TOP - PAGE_SIZE;
    secs.stack_end = STACK_TOP;
    set_sections(&new_pgdir, secs);

    for (int i = 0; i < elf_header.e_phnum; i++) {
        if (p_header[i].p_type == (u32)PT_LOAD) {
            load_into_memory(&new_pgdir, ip, secs, p_header[i]);
        }
    }
    u64 sp = secs.stack_end;
    u64 args_addr[MAX_ARGC];
    u64 envps_addr[MAX_ARGC];
    vmmap(&new_pgdir, sp, kalloc_page(), PTE_RW | PTE_VALID | PTE_USER_DATA);
    vmmap(&new_pgdir, sp - PAGE_SIZE, kalloc_page(),
          PTE_RW | PTE_VALID | PTE_USER_DATA);
    isize argc = 0;
    isize envc = 0;
    while (envp != NULL && envp[envc] != NULL) {
        // 复制argv[n-1] ~ argv[0]的字符串
        sp -= eight_ceil(strlen(envp[envc]) + 1);
        envps_addr[envc] = sp;
        copyout(&new_pgdir, (void *)sp, envp[envc], strlen(envp[envc]) + 1);
        envc++;
    }

    while (argv != NULL && argv[argc] != NULL) {
        // 复制argv[n-1] ~ argv[0]的字符串
        sp -= eight_ceil(strlen(argv[argc]) + 1);
        args_addr[argc] = sp;
        copyout(&new_pgdir, (void *)sp, argv[argc], strlen(argv[argc]) + 1);
        argc++;
    }
    void *not_aligned_final_sp = (void *)sp - 8 * argc - 8 * envc - 8;
    if (((u64)not_aligned_final_sp & 0xf) != 0) {
        // 16字节对齐
        sp -= 8;
    }
    for (int i = envc - 1; i >= 0; i--) {
        sp -= 8;
        copyout(&new_pgdir, (void *)sp, &envps_addr[i], 8);
    }
    for (int i = argc - 1; i >= 0; i--) {
        // 复制argv[n-1] ~ argv[0]的地址
        sp -= 8;
        copyout(&new_pgdir, (void *)sp, &args_addr[i], 8);
    }
    sp -= 8;
    copyout(&new_pgdir, (void *)sp, &argc, 8);
    UserContext *user_context = thisproc()->ucontext;
    user_context->elr = elf_header.e_entry;
    user_context->x[0] = argc;
    user_context->x[1] = sp + 8;
    user_context->x[2] = sp + 8 + 8 * argc;
    user_context->sp_el0 = sp;
    // if (argv[0][0] == 'u') {
    //     PANIC();
    // }
    free_pgdir(&thisproc()->pgdir);
    memcpy(&thisproc()->pgdir, &new_pgdir, sizeof(struct pgdir));
    auto p = &thisproc()->pgdir;
    // 由于new_pgdir里的section_head的地址与thisproc()->pgdir里的section_head的地址不同，所以需要修改section_head
    p->section_head.prev = p->section_head.next->next->next->next->next;
    p->section_head.next->next->next->next->next->next = &p->section_head;
    attach_pgdir(p);
    arch_tlbi_vmalle1is();
    return 0;
}
