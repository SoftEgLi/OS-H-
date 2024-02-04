#include <driver/sd.h>
#include <kernel/cpu.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <test/test.h>
bool panic_flag;
void trap_return();
int execve(const char *path, char *const argv[], char *const envp[]);

NO_RETURN void idle_entry() {
    set_cpu_on();
    while (1) {
        yield();
        if (panic_flag)
            break;
        arch_with_trap { arch_wfi(); }
    }
    set_cpu_off();
    arch_stop_cpu();
}
extern char icode[], eicode[];
NO_RETURN void kernel_entry() {
    printk("hello world %d\n", (int)sizeof(struct proc));
    printk("%lld\n", (u64)sizeof(struct proc));
    // proc_test();
    // vm_test();
    // user_proc_test();
    // sd_init();
    // sd_test();
    // pgfault_first_test();
    // pgfault_second_test();

    do_rest_init();
    // TODO: map init.S to user space and trap_return to run icode
    struct proc *p = create_proc();
    for (u64 q = (u64)icode; q < (u64)eicode; q += PAGE_SIZE) {
        *get_pte(&p->pgdir, PAGE_SIZE + q - (u64)icode, true) =
            K2P(q) | PTE_VALID | PTE_RX | PTE_USER_DATA;
        // vmmap(&p->pgdir, +q - (u64)icode, (void *)q, PTE_VALID | PTE_RX);
    }
    p->ucontext->x[0] = 0;
    p->ucontext->elr = PAGE_SIZE;
    p->ucontext->spsr = 0x0;
    start_proc(p, trap_return, 0);
    while (1) {
        yield();
        arch_with_trap { arch_wfi(); }
    }
}

NO_INLINE NO_RETURN void _panic(const char *file, int line) {
    printk("=====%s:%d PANIC%d!=====\n", file, line, cpuid());
    panic_flag = true;
    set_cpu_off();
    for (int i = 0; i < NCPU; i++) {
        if (cpus[i].online)
            i--;
    }
    printk("Kernel PANIC invoked at %s:%d. Stopped.\n", file, line);
    arch_stop_cpu();
}
