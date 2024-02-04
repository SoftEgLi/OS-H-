#include <aarch64/intrinsic.h>
#include <common/sem.h>
#include <common/spinlock.h>
#include <driver/interrupt.h>
#include <driver/uart.h>
#include <kernel/console.h>
#include <kernel/init.h>
#include <kernel/sched.h>

#define INPUT_BUF 128
#define BACKSPACE 0x100
usize wrap_inc(usize x) { return (x + 1) % INPUT_BUF; }
usize wrap_dec(usize x) { return (x + INPUT_BUF - 1) % INPUT_BUF; }
struct {
    SpinLock lock;
    Semaphore read;

    char buf[INPUT_BUF];
    usize r; // Read index
    usize w; // Write index
    usize e; // Edit index
} input;

#define C(x) ((x) - '@') // Control-x

void console_put_char(int c) {
    if (c == BACKSPACE) {
        // if the user typed backspace, overwrite with a space.
        uart_put_char('\b');
        uart_put_char(' ');
        uart_put_char('\b');
    } else {
        uart_put_char(c);
    }
}

define_rest_init(cons_input_init) {
    set_interrupt_handler(IRQ_AUX, console_intr);
    init_spinlock(&input.lock);
    init_sem(&input.read, 0);
}

isize console_write(Inode *ip, char *buf, isize n) {
    // TODO
    if (ip) {
    }
    _acquire_spinlock(&input.lock);
    for (int i = 0; i < n; i++) {
        uart_put_char(buf[i]);
    }
    _release_spinlock(&input.lock);
    return n;
}

isize console_read(Inode *ip, char *dst, isize n) {
    // TODO
    if (ip) {
    }
    char c = 0;
    isize count = n;
    _acquire_spinlock(&input.lock);
    while (n > 0) {
        while (input.r == input.w) {
            _lock_sem(&input.read);
            _release_spinlock(&input.lock);
            if (_wait_sem(&input.read, true) == false) {
                // inodes.lock(ip);
                return -1;
            }
            _acquire_spinlock(&input.lock);
        }

        c = input.buf[input.r];
        if (c == C('D')) { // end-of-file
            // Save ^D for next time, to make sure caller gets a 0-byte result.
            break;
        }
        input.r = wrap_inc(input.r);

        *(dst++) = c;
        --n;

        if (c == '\n') {
            break;
        }
    }
    if (input.buf[input.r] == C('D')) {
        input.r = wrap_inc(input.r);
    }
    _release_spinlock(&input.lock);
    return count - n;
}

void console_intr() {
    // TODO
    _acquire_spinlock(&input.lock);

    bool is_empty = true;
    char c;
    while ((c = uart_get_char())) {
        if (c == (char)-1) {
            break;
        }
        switch (c) {
        case C('U'):
            while (input.e != input.w && input.buf[wrap_dec(input.e)] != '\n') {
                input.e = wrap_dec(input.e);
                console_put_char(BACKSPACE);
            }
            break;
        case '\x7f':
            if (input.e != input.w) {
                input.e = wrap_dec(input.e);
                console_put_char(BACKSPACE);
            }
            break;
        default:
            if ((c != 0) && (input.e != input.r || is_empty)) {
                is_empty = false;
                c = (c == '\r') ? '\n' : c;
                input.buf[input.e] = c;
                input.e = wrap_inc(input.e);
                console_put_char(c);

                if (c == '\n' || c == C('D') || input.e == input.w) {
                    input.w = input.e;
                    post_all_sem(&input.read);
                }
            }
            break;
        }
    }
    _release_spinlock(&input.lock);
}
