// Do kernel-mode context switch
// x0 (first parameter): new context ptr
// x1 (second parameter): addr to save old context ptr

#define pushp(a, b) stp a, b, [sp, #-0x10]!
#define popp(a, b) ldp a, b, [sp], #0x10 

.globl swtch
swtch:
    pushp(x29,xzr)
    pushp(x27,x28)
    pushp(x25,x26)
    pushp(x23,x24)
    pushp(x21,x22)
    pushp(x19,x20)
    pushp(x17,x18)
    pushp(x15,x16)
    pushp(x13,x14)
    pushp(x11,x12)
    pushp(x9,x10)
    pushp(x7,x8)
    pushp(x5,x6)
    pushp(x3,x4)
    pushp(x1,x2)
    pushp(lr,x0)
    mov x2,sp
    str x2,[x1]
    mov sp,x0
    popp(lr,x0)
    popp(x1,x2)
    popp(x3,x4)
    popp(x5,x6)
    popp(x7,x8)
    popp(x9,x10)
    popp(x11,x12)
    popp(x13,x14)
    popp(x15,x16)
    popp(x17,x18)
    popp(x19,x20)
    popp(x21,x22)
    popp(x23,x24)
    popp(x25,x26)
    popp(x27,x28)
    popp(x29,xzr)
    ret
// TODO

