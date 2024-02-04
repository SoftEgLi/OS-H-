
#include <driver/sddef.h>
#include <kernel/mem.h>
/*
 * Initialize SD card.
 * Returns zero if initialization was successful, non-zero otherwise.
 */
int sdInit();
/*
Wait for interrupt.
return after interrupt handling
*/
static int sdWaitForInterrupt(unsigned int mask);
/*
data synchronization barrier.
use before access memory
*/
static ALWAYS_INLINE void arch_dsb_sy();
/*
call handler when interrupt
*/
void set_interrupt_handler(InterruptType type, InterruptHandler handler);
/*

*/
ALWAYS_INLINE u32 get_EMMC_DATA() { return *EMMC_DATA; }
ALWAYS_INLINE u32 get_and_clear_EMMC_INTERRUPT() {
    u32 t = *EMMC_INTERRUPT;
    *EMMC_INTERRUPT = t;
    return t;
}

/*
 * Initialize SD card and parse MBR.
 * 1. The first partition should be FAT and is used for booting.
 * 2. The second partition is used by our file system.
 *
 * See https://en.wikipedia.org/wiki/Master_boot_record
 */
static SpinLock sdLock;
static Queue sdQueue;
static SpinLock intrLock;
Semaphore sdSem;

u32 sd_init() {
    /*
     * 1.call sdInit.
     * 2.Initialize the lock and request queue if any.
     * 3.Read and parse 1st block (MBR) and collect whatever
     * information you want.
     * 4.set interrupt handler for IRQ_SDIO,IRQ_ARASANSDIO
     *
     * Hint:
     * 1.Maybe need to use sd_start for reading, and
     * sdWaitForInterrupt for clearing certain interrupt.
     * 2.Remember to call sd_init() at somewhere.
     * 3.the first number is 0.
     * 4.don't forget to call this function somewhere
     * TODO: Lab5 driver.
     */
    if (!sdInit()) {
        printk("sd init failed\n");
        PANIC();
    }

    set_interrupt_handler(IRQ_SDIO, sd_intr);
    set_interrupt_handler(IRQ_ARASANSDIO, sd_intr);
    init_spinlock(&sdLock);
    init_spinlock(&intrLock);
    queue_init(&sdQueue);
    buf b;
    b.blockno = 0;
    b.flags = 0;
    sdrw(&b);
    int *lba;
    int *sector_count;
    lba = (int *)(b.data + 0x1CE + 8);
    sector_count = (int *)(b.data + 0x1CE + 12);

    printk("lba:%d,sec:%d\n", *lba, *sector_count);
    return (usize)(*lba);
    // if (sdWaitForInterrupt(INT_DATA_DONE))
    // {
    //     printk("sd init failed\n");
    //     PANIC();
    // }
}

/* Start the request for b. Caller must hold sdlock. */
static void sd_start(struct buf *b) {
    // Address is different depending on the card type.
    // HC pass address as block #.
    // SC pass address straight through.
    int bno =
        sdCard.type == SD_TYPE_2_HC ? (int)b->blockno : (int)b->blockno << 9;
    int write = b->flags & B_DIRTY;

    // printk("- sd start: cpu %d, flag 0x%x, bno %d, write=%d\n", cpuid(),
    // b->flags, bno, write);

    // Ensure that any data operation has completed before doing the transfer.
    if (*EMMC_INTERRUPT) {
        printk("emmc interrupt flag should be empty: 0x%x. \n",
               *EMMC_INTERRUPT);
        PANIC();
    }

    // Work out the status, interrupt and command values for the transfer.
    int cmd = write ? IX_WRITE_SINGLE : IX_READ_SINGLE;

    int resp;
    *EMMC_BLKSIZECNT = 512;

    if ((resp = sdSendCommandA(cmd, bno))) {
        printk("* EMMC send command error.\n");
        PANIC();
    }

    int done = 0;
    u32 *intbuf = (u32 *)b->data;
    if (!(((i64)b->data) & 0x03) == 0) {
        printk("Only support word-aligned buffers. \n");
        PANIC();
    }

    if (write) {
        // Wait for ready interrupt for the next block.
        if ((resp = sdWaitForInterrupt(INT_WRITE_RDY))) {
            printk("* EMMC ERROR: Timeout waiting for ready to write\n");
            PANIC();
            // return sdDebugResponse(resp);
        }
        if (*EMMC_INTERRUPT) {
            printk("%d\n", *EMMC_INTERRUPT);
            PANIC();
        }
        while (done < 128)
            *EMMC_DATA = intbuf[done++];
    }
}

/* The interrupt handler. Sync buf with disk.*/
void sd_intr() {

    /*
     * Pay attention to whether there is any element in the buflist.
     * Understand the meanings of EMMC_INTERRUPT, EMMC_DATA, INT_DATA_DONE,
     * INT_READ_RDY, B_DIRTY, B_VALID and some other flags.
     *
     * Notice that reading and writing are different, you can use flags
     * to identify.
     *
     * If B_DIRTY is set, write buf to disk, clear B_DIRTY, set B_VALID.
     * Else if B_VALID is not set, read buf from disk, set B_VALID.
     *
     * Remember to clear the flags after reading/writing.
     *
     * When finished, remember to use pop and check whether the list is
     * empty, if not, continue to read/write.
     *
     * You may use some buflist functions, arch_dsb_sy(), sd_start(), post_sem()
     * and sdWaitForInterrupt() to complete this function.
     *
     * TODO: Lab5 driver.
     */
    queue_lock(&sdQueue);
    ListNode *l = queue_front(&sdQueue);
    buf *b = container_of(l, buf, listNode);
    queue_pop(&sdQueue);
    // printk("sd_intr:%d\n", *EMMC_INTERRUPT);

    ASSERT(&b->sem != 0);
    if (b == NULL) {
        printk("sd intr error\n");
        PANIC();
    }
    if (b->flags & B_DIRTY) {
        // printk("begin Dirty\n");
        sdWaitForInterrupt(INT_DATA_DONE);
        b->flags &= ~B_DIRTY;
        b->flags |= B_VALID;
        post_sem(&b->sem);
        // printk("End post_Sem\n");

        // printk("End Dirty\n");
    } else if (!(b->flags & B_VALID)) {
        if (sdWaitForInterrupt(INT_READ_RDY)) {
            printk("sd read_RDY error\n");
            PANIC();
        }
        u32 *p = (u32 *)b->data;

        for (int i = 0; i < 128; i++)
            p[i] = *EMMC_DATA;
        if (sdWaitForInterrupt(INT_DATA_DONE)) {
            printk("sd data_DONE error\n");
            PANIC();
        }
        b->flags |= B_VALID;

        post_sem(&b->sem);
        // printk("End post_Sem\n");
    } else {
        printk("Error\n");
    }
    if (!queue_empty(&sdQueue)) {
        _acquire_spinlock(&sdLock);

        ListNode *head = queue_front(&sdQueue);
        buf *b2 = container_of(head, buf, listNode);
        queue_unlock(&sdQueue);
        sd_start(b2);

        _release_spinlock(&sdLock);
    }
    queue_unlock(&sdQueue);
    // printk("End Handler\n");
}

void sdrw(buf *b) {
    /*
     * 1.add buf to the queue
     * 2.if no buf in queue before,send request now
     * 3.'loop' until buf flag is modified
     *
     * You may use some buflist functions, arch_dsb_sy(),
     * sd_start(), wait_sem() to complete this function.
     *  TODO: Lab5 driver.
     */
    int old_flags = b->flags;
    while (b->flags == old_flags) {
        init_sem(&b->sem, 0);
        // enqueue(&buf_queue, &b->bnode);
        queue_lock(&sdQueue);
        queue_push(&sdQueue, &b->listNode);
        arch_dsb_sy();
        if (sdQueue.sz == 1) {
            queue_unlock(&sdQueue);
            _acquire_spinlock(&sdLock);
            sd_start(b);
            _release_spinlock(&sdLock);
        } else {
            queue_unlock(&sdQueue);
        }
        // if(!wait_sem(&b->bsem)) PANIC();
        _lock_sem(&b->sem);
        bool ret = _wait_sem(&b->sem, false);
        ASSERT(ret);
        // ASSERT(_wait_sem(&b->sem, false));
        // ASSERT((_lock_sem(sem), _wait_sem(sem, false)));
        // unalertable_wait_sem(&b->sem);
    }
}

/* SD card test and benchmark. */
void sd_test() {
    static struct buf b[1 << 11];
    int n = sizeof(b) / sizeof(b[0]);
    int mb = (n * BSIZE) >> 20;
    // assert(mb);
    if (!mb)
        PANIC();
    i64 f, t;
    asm volatile("mrs %[freq], cntfrq_el0" : [freq] "=r"(f));
    printk("- sd test: begin nblocks %d\n", n);

    printk("- sd check rw...\n");
    // Read/write test
    for (int i = 1; i < n; i++) {
        // Backup.
        b[0].flags = 0;
        b[0].blockno = (u32)i;
        sdrw(&b[0]);
        // Write some value.
        b[i].flags = B_DIRTY;
        b[i].blockno = (u32)i;
        for (int j = 0; j < BSIZE; j++)
            b[i].data[j] = (u8)((i * j) & 0xFF);
        sdrw(&b[i]);

        memset(b[i].data, 0, sizeof(b[i].data));
        // Read back and check
        b[i].flags = 0;
        sdrw(&b[i]);
        for (int j = 0; j < BSIZE; j++) {
            //   assert(b[i].data[j] == (i * j & 0xFF));
            if (b[i].data[j] != (i * j & 0xFF))
                PANIC();
        }
        // Restore previous value.
        b[0].flags = B_DIRTY;
        sdrw(&b[0]);
    }
    printk("Part1 Done\n");
    // Read benchmark
    arch_dsb_sy();
    t = (i64)get_timestamp();
    arch_dsb_sy();
    for (int i = 0; i < n; i++) {
        b[i].flags = 0;
        b[i].blockno = (u32)i;
        sdrw(&b[i]);
    }
    arch_dsb_sy();
    t = (i64)get_timestamp() - t;
    arch_dsb_sy();
    printk("- read %dB (%dMB), t: %lld cycles, speed: %lld.%lld MB/s\n",
           n * BSIZE, mb, t, mb * f / t, (mb * f * 10 / t) % 10);

    // Write benchmark
    arch_dsb_sy();
    t = (i64)get_timestamp();
    arch_dsb_sy();
    for (int i = 0; i < n; i++) {
        b[i].flags = B_DIRTY;
        b[i].blockno = (u32)i;
        sdrw(&b[i]);
    }
    arch_dsb_sy();
    t = (i64)get_timestamp() - t;
    arch_dsb_sy();

    printk("- write %dB (%dMB), t: %lld cycles, speed: %lld.%lld MB/s\n",
           n * BSIZE, mb, t, mb * f / t, (mb * f * 10 / t) % 10);
}