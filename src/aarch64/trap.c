#include <aarch64/intrinsic.h>
#include <aarch64/trap.h>
#include <driver/interrupt.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

void trap_global_handler(UserContext *context) {
    // static int syscall_count = 0;
    thisproc()->ucontext = context;
    u64 esr = arch_get_esr();
    u64 ec = esr >> ESR_EC_SHIFT;
    u64 iss = esr & ESR_ISS_MASK;
    u64 ir = esr & ESR_IR_MASK;
    arch_reset_esr();
    // unsigned long long int elr_el1, far_el1;
    // asm volatile("mrs %0, elr_el1" : "=r"(elr_el1));
    // asm volatile("mrs %0, far_el1" : "=r"(far_el1));
    // printk("elr_el1 = %llx\n", elr_el1);
    // printk("far_el1 = %llx\n", far_el1);
    // printk("sp_el0 = %llx\n", context->sp_el0);
    switch (ec) {
    case ESR_EC_UNKNOWN: {
        if (ir) {
            unsigned long long int elr_el1, far_el1;
            asm volatile("mrs %0, elr_el1" : "=r"(elr_el1));
            asm volatile("mrs %0, far_el1" : "=r"(far_el1));
            printk("About to PANIC\n");
            printk("elr_el1 = %llx\n", elr_el1);
            printk("far_el1 = %llx\n", far_el1);
            printk("sp_el0 = %llx\n", context->sp_el0);
            printk("x19 = %llx\n", context->x[19]);
            printk("x20 = %llx\n", context->x[20]);
            printk("lr = %llx\n", context->x[30]);
            printk("Broken pc?\n");
            PANIC();
        } else {
            interrupt_global_handler();
        }
    } break;
    case ESR_EC_SVC64: {
        // printk("%llx\n", context->x[0]);
        // printk("系统调用一次\n");
        // printk("id = %lld\n", context->x[8]);
        // syscall_count++;
        // // ASSERT(syscall_count != 2);
        syscall_entry(context);
    } break;
    case ESR_EC_IABORT_EL0:
    case ESR_EC_IABORT_EL1:
    case ESR_EC_DABORT_EL0:
    case ESR_EC_DABORT_EL1: {
        // unsigned long long int elr_el1_value, far_el1_value, lr;
        // asm volatile("mrs %0, elr_el1" : "=r"(elr_el1_value));
        // asm volatile("mrs %0, far_el1" : "=r"(far_el1_value));
        // asm volatile("mov %0, x30" : "=r"(lr));
        // printk("Pagefault_handler:elr_el1 = %llx,far_el1 = %llx,lr = %llx\n",
        //        elr_el1_value, far_el1_value, lr);
        // if (elr_el1_value == 0) {
        //     PANIC();
        // }
        pgfault_handler(iss);
    } break;
    default: {
        printk("Unknwon exception %llu\n", ec);
        PANIC();
    }
    }

    // TODO: stop killed process while returning to user space
    if (thisproc()->killed && !(context->spsr & 0x10)) {
        exit(-1);
    }
    // asm volatile("msr far_el1,xzr");
}

NO_RETURN void trap_error_handler(u64 type) {
    printk("Unknown trap type %llu\n", type);
    PANIC();
}
