#include <aarch64/intrinsic.h>
#include <driver/clock.h>
#include <kernel/cpu.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
// #define DEBUG
extern bool panic_flag;

extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);

ListNode rq;
u64 rq_count;
// ListNode *tail;
SpinLock rqLock;
extern struct timer my_timer[NCPU];
define_early_init(rq) {
    init_spinlock(&rqLock);
    init_list_node(&rq);
    rq_count = 0;
    // tail = &rq;
    panic_flag = false;
}

define_init(sched) {
    for (int i = 0; i < NCPU; i++) {
        struct proc *p = kalloc(sizeof(struct proc));
        p->idle = 1;
        p->pid = i;
        p->state = RUNNING;
        cpus[i].sched.thisproc = cpus[i].sched.idle = p;
    }
}

struct proc *thisproc() {
    // TODO: return the current process
    return cpus[cpuid()].sched.thisproc;
}

void init_schinfo(struct schinfo *p) {
    // TODO: initialize your customized schinfo for every newly-created process
    init_list_node(&p->rq);
}

void _acquire_sched_lock() {
    // TODO: acquire the sched_lock if need
    _acquire_spinlock(&rqLock);
}

void _release_sched_lock() {
    // TODO: release the sched_lock if need
    _release_spinlock(&rqLock);
}

bool is_zombie(struct proc *p) {
    bool r;
    _acquire_sched_lock();
    r = p->state == ZOMBIE;
    _release_sched_lock();
    return r;
}

bool is_unused(struct proc *p) {
    bool r;
    _acquire_sched_lock();
    r = p->state == UNUSED;
    _release_sched_lock();
    return r;
}

bool _activate_proc(struct proc *p, bool onalert) {
    _acquire_sched_lock();
    // TODO
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE
    // and add it to the sched queue else: panic printk("activate_proc:%d,state
    // = %d,idle = %d\n", p->pid, p->state,p->idle);
    if (p->state == RUNNING || p->state == RUNNABLE) {
        _release_sched_lock();
        return false;
    } else if (p->state == SLEEPING || p->state == UNUSED) {
        p->state = RUNNABLE;
        rq_count++;
        _insert_into_list(&rq, &p->schinfo.rq);
        // tail = &p->schinfo.rq;
    } else if (p->state == ZOMBIE) {
        p->state = RUNNABLE;
        rq_count++;
        _insert_into_list(&rq, &p->schinfo.rq);
    } else if (p->state == DEEPSLEEPING) {
        if (!onalert) {
            p->state = RUNNABLE;
            rq_count++;
            _insert_into_list(&rq, &p->schinfo.rq);
        } else {
            _release_sched_lock();
            return false;
        }
    } else {
        PANIC();
    }
    _release_sched_lock();
    return true;
}

static void update_this_state(enum procstate new_state) {
    // TODO: if using simple_sched, you should implement this routinue
    // update the state of current process to new_state, and remove it from the
    // sched queue if new_state=SLEEPING/ZOMBIE

    struct proc *thisproc = cpus[cpuid()].sched.thisproc;
    //
    //
    thisproc->state = new_state;
    if (new_state == SLEEPING || new_state == DEEPSLEEPING ||
        new_state == ZOMBIE) {
        rq_count--;
        _detach_from_list(&thisproc->schinfo.rq);
    }
}

static struct proc *pick_next() {
    // TODO: if using simple_sched, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process
    if (panic_flag) {
        return cpus[cpuid()].sched.idle;
    }
    u64 idx = my_timer[cpuid()].data % rq_count;
    u64 i = -1;
    struct proc *next_proc = NULL;
    _for_in_list(p, &rq) {
        i++;
        if (p == &rq) {
            continue;
        }
        auto temp = container_of(p, struct proc, schinfo.rq);
        if (temp->state == RUNNABLE) {
            next_proc = temp;

            if (i == idx) {
                // printk("CPU %d:pid = %d\n", cpuid(), next_proc->pid);
                return next_proc;
            }
        }
    }

    if (next_proc)
        return next_proc;
    return cpus[cpuid()].sched.idle;
}
extern struct timer my_timer[NCPU];
static void update_this_proc(struct proc *p) {
    // TODO: if using simple_sched, you should implement this routinue
    // update thisproc to the choosen process, and reset the clock interrupt if
    // need

    struct timer *timer = &my_timer[cpuid()];
    if (!timer->triggered) {
        cancel_cpu_timer(timer);
    }
    set_cpu_timer(timer);
    cpus[cpuid()].sched.thisproc = p;
    // reset_clock(100);
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
static void simple_sched(enum procstate new_state) {
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    if (this->killed == 1 && new_state != ZOMBIE) {
        _release_sched_lock();
        return;
    }
    update_this_state(new_state);
    auto next = pick_next();
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this) {
        attach_pgdir(&next->pgdir);
        // ASSERT(this->pid != 5);
        // printk("cpu%d:pid = %d,idle =%d\n", cpuid(), next->pid, next->idle);
        // if (!next->idle) {
        //     printk("elr = %llx\n", next->ucontext->elr);
        // } else {
        //     printk("\n");
        // }
        // printk("Got the user_proc\n");
        swtch(next->kcontext, &this->kcontext);
    }
    _release_sched_lock();
}

__attribute__((weak, alias("simple_sched"))) void
_sched(enum procstate new_state);

u64 proc_entry(void (*entry)(u64), u64 arg) {
    _release_sched_lock();
    set_return_addr(entry);
    return arg;
}
