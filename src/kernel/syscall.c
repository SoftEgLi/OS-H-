#include <common/sem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/pt.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
void *syscall_table[NR_SYSCALL];

void syscall_entry(UserContext *context) {
    // TODO
    // Invoke syscall_table[id] with args and set the return value.
    // id is stored in x8. args are stored in x0-x5. return value is stored in
    // x0.
    u64 id = 0, ret = 0;
    id = context->x[8];
    if (id < NR_SYSCALL) {
        u64 (*func)(u64, u64, u64, u64, u64, u64);
        func = syscall_table[id];
        // printk("syscall_table[%lld] = %p\n", id, func);
        // if (id == 221) {
        //     printk("path = %llx\n", (u64)context->x[0]);
        //     printk("argv = %llx\n", (u64)context->x[1]);
        //     printk("envp = %llx\n", (u64)context->x[2]);
        // }
        // if (id == 63) {
        //     printk("Here\n");
        // }
        ret = func(context->x[0], context->x[1], context->x[2], context->x[3],
                   context->x[4], context->x[5]);
        context->x[0] = ret;
    }
}
// check if the virtual address [start,start+size) is READABLE by the current
// user process
bool user_readable(const void *start, usize size) {
    // TODO

    const void *start_addr = start;
    while ((isize)size > 0) {
        struct section *sec = get_section_by_va((u64)start_addr);
        if (sec == NULL) {
            PANIC();
            return false;
        }
        start_addr += PAGE_SIZE;
        size -= PAGE_SIZE;
    }
    return true;
}

// check if the virtual address [start,start+size) is READABLE & WRITEABLE by
// the current user process
bool user_writeable(const void *start, usize size) {
    // TODO
    isize count = (isize)size;
    const void *start_addr = start;
    while ((isize)count > 0) {
        struct section *sec = get_section_by_va((u64)start_addr);
        if (sec == NULL) {
            PANIC();
            return false;
        }
        if (sec->flags == ST_TEXT) {
            return false;
        }
        start_addr += PAGE_SIZE;
        count -= PAGE_SIZE;
    }
    return true;
}

// get the length of a string including tailing '\0' in the memory space of
// current user process return 0 if the length exceeds maxlen or the string is
// not readable by the current user process
usize user_strlen(const char *str, usize maxlen) {
    for (usize i = 0; i < maxlen; i++) {
        if (user_readable(&str[i], 1)) {
            if (str[i] == 0)
                return i + 1;
        } else
            return 0;
    }
    return 0;
}