#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block device and super block to use.
            Correspondingly, you should NEVER use global instance of
            them, e.g. `get_super_block`, `block_device`

    @see init_bcache
 */
static const SuperBlock *sblock;

/**
    @brief the reference to the underlying block device.
 */
static const BlockDevice *device;
// static BitmapCell **bitmap;
/**
    @brief global lock for block cache.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, etc.
 */
static SpinLock cache_lock;

/**
    @brief the list of all allocated in-memory block.

    We use a linked list to manage all allocated cached blocks.

    You can implement your own data structure if you like better performance.

    @see Block
 */
static ListNode head;
static usize num_cached_blocks;
static LogHeader header; // in-memory copy of log header block.
/**
    @brief a struct to maintain other logging states.

    You may wonder where we store some states, e.g.

    * how many atomic operations are running?
    * are we checkpointing?
    * how to notify `end_op` that a checkpoint is done?

    Put them here!

    @see cache_begin_op, cache_end_op, cache_sync
 */
struct {
    /* your fields here */
    SpinLock lock;           // 当操作日志时，需要加锁
    Semaphore sem;           // 保证进行checkpoint时不开启事务
    int operation_count;     // 当前checkpoint正在执行的FS操作数量
    bool is_committing;      // 是否正在commit
    int started_event_count; // 当前checkpoint事务的数量
    Semaphore end_op_sem;    // 用于通知end_op，checkpoint已经开始
} log;

// read the content from disk.
static INLINE void device_read(Block *block) {
    device->read(block->block_no, block->data);
}

// write the content back to disk.
static INLINE void device_write(Block *block) {
    device->write(block->block_no, block->data);
}

// read log header from disk.
static INLINE void read_header() {
    device->read(sblock->log_start, (u8 *)&header);
}

// write log header back to disk.
static INLINE void write_header() {
    device->write(sblock->log_start, (u8 *)&header);
}

// initialize a block struct.
static void init_block(Block *block) {
    //-------------------My init------------------
    block->refcnt = 0;
    //--------------------------------------------
    block->block_no = 0;
    init_list_node(&block->node);
    block->acquired = false;
    block->pinned = false;

    init_sleeplock(&block->lock);
    block->valid = false;
    memset(block->data, 0, sizeof(block->data));
}
// see `cache.h`.
static usize get_num_cached_blocks() {
    // TODO
    return num_cached_blocks;
}
//  see `cache.h`.
static Block *cache_acquire(usize block_no) {
    // TODO
    _acquire_spinlock(&cache_lock);

    ListNode *b = head.next;
    // 第一步，遍历cache列表，如果有，则直接返回
    for (; b != &head; b = b->next) {
        Block *block = container_of(b, Block, node);
        if (block->block_no == block_no) {
            block->refcnt++;
            _release_spinlock(&cache_lock);
            unalertable_acquire_sleeplock(&block->lock);
            block->acquired = true;
            return block;
        }
    }
    // 第二步，没有找到，如果cache的数量小于软限制，那么会分配一个新的block，并且插入到链表头
    if (num_cached_blocks < EVICTION_THRESHOLD) {
        Block *block = kalloc(sizeof(Block));
        init_block(block);
        block->block_no = block_no;
        block->refcnt = 1;
        _insert_into_list(&head, &block->node);
        num_cached_blocks++;
        _release_spinlock(&cache_lock);
        device_read(block);
        block->valid = 1;
        unalertable_acquire_sleeplock(&block->lock);
        block->acquired = 1;
        return block;
    }
    // 没有找到，并且数量大于等于软限制，那么会遍历整个链表，找到一个空闲的block，

    for (b = head.prev; b != &head; b = b->prev) {
        Block *block = container_of(b, Block, node);
        if (block->refcnt == 0 && block->pinned == false) {
            if (block->acquired) {
                // 错误检查
                PANIC();
            }
            block->block_no = block_no;
            block->valid = 0;
            block->refcnt = 1;
            init_sleeplock(&block->lock);
            _release_spinlock(&cache_lock);
            device_read(block);
            block->valid = 1;
            unalertable_acquire_sleeplock(&block->lock);
            block->acquired = 1;
            return block;
        }
    }
    // 如果找不到，那么会新建一个block，插入到链表头
    Block *block = kalloc(sizeof(Block));
    init_block(block);
    block->block_no = block_no;
    block->refcnt = 1;
    _insert_into_list(&head, &block->node);
    num_cached_blocks++;
    _release_spinlock(&cache_lock);
    device_read(block);
    // arch_with_trap { device_read(block); }

    block->valid = 1;
    unalertable_acquire_sleeplock(&block->lock);
    block->acquired = 1;
    return block;
}

// see `cache.h`.
static void cache_release(Block *block) {
    // TODO
    // 第一步，释放睡眠锁
    block->acquired = false;
    release_sleeplock(&block->lock);
    // 第二步，如果refcnt为0，并且不是脏快，并且cache_block的数量大于软限制，那么释放该block
    _acquire_spinlock(&cache_lock);
    block->refcnt--;
    if (block->refcnt == 0 && block->pinned == false &&
        num_cached_blocks > EVICTION_THRESHOLD) {
        _detach_from_list(&block->node);
        num_cached_blocks--;
        kfree(block);
        _release_spinlock(&cache_lock);
        return;
    }
    // 如果refcnt为0，并且不是脏快，并且cache_block的数量小于软限制，那么将该block插入到链表头
    if (block->refcnt == 0 && block->pinned == false &&
        num_cached_blocks <= EVICTION_THRESHOLD) {
        _detach_from_list(&block->node);
        _insert_into_list(&head, &block->node);
        _release_spinlock(&cache_lock);
        return;
    }
    _release_spinlock(&cache_lock);
}

void restore_log() {
    read_header();
    if (header.valid == true || header.num_blocks != 0) {
        for (usize i = 0; i < header.num_blocks; i++) {
            Block *block = kalloc(sizeof(Block));
            block->block_no = sblock->log_start + 1 + i;
            device_read(block);
            block->block_no = header.block_no[i];
            device_write(block);
        }
        header.num_blocks = 0;
        header.valid = false;
        write_header();
    }
}
void init_log() {
    init_spinlock(&log.lock);
    init_sem(&log.sem, 0);
    init_sem(&log.end_op_sem, 0);
    log.operation_count = 0;
    log.is_committing = false;
    log.started_event_count = 0;
}
void init_log_header() {
    header.num_blocks = 0;
    header.valid = false;
}
// see `cache.h`.
void init_bcache(const SuperBlock *_sblock, const BlockDevice *_device) {
    sblock = _sblock;
    device = _device;

    // TODO
    // 初始化锁
    init_spinlock(&cache_lock);
    init_list_node(&head);
    num_cached_blocks = 0;
    // init_memory_bitmap();
    restore_log();
    init_log();
    init_log_header();
}
// see `cache.h`.

void init_ctx(OpContext *ctx) {
    init_spinlock(&ctx->lock);
    ctx->rm = OP_MAX_NUM_BLOCKS;
    ctx->done = 0;
}

static void cache_begin_op(OpContext *ctx) {
    // TODO
    // 第一步，获取日志锁
    // 第二步，如果正在committing，那么就陷入睡眠
    //      如果加上本次操作的事务数量，outstanding的数量超过阈值，那么同样陷入睡眠
    // 如果上述情况不出现，那么就把本次操作的事务数量加上
    // 第三步，释放锁
    // 第四步，将中断关闭，防止中途陷入时钟中断
    // _acquire_spinlock(&ctx->lock);
    init_ctx(ctx);
    _acquire_spinlock(&log.lock);
    while (log.is_committing == true ||
           (log.operation_count + ctx->rm > (sblock->num_log_blocks - 1)) ||
           (log.operation_count + ctx->rm > LOG_MAX_SIZE)) {
        _lock_sem(&log.sem);
        _release_spinlock(&log.lock);
        if (_wait_sem(&log.sem, false)) {
        }

        _acquire_spinlock(&log.lock);
    }
    log.started_event_count++;
    log.operation_count += ctx->rm;
    _release_spinlock(&log.lock);
}

// see `cache.h`.
static void cache_sync(OpContext *ctx, Block *block) {
    // TODO
    if (ctx == NULL) {
        // 如果ctx为空，那么直接写入磁盘
        printk("sync null\n");
        device_write(block);
        block->pinned = false;
        return;
    }
    if (block->pinned == true) {
        return;
    }
    block->pinned = true;
    _acquire_spinlock(&ctx->lock);
    ctx->rm--;
    ctx->done++;
    if (ctx->rm + ctx->done != OP_MAX_NUM_BLOCKS) {
        _release_spinlock(&ctx->lock);
        printk("ctx->rm = %lld,ctx->done = %lld\n", ctx->rm, ctx->done);
        PANIC();
    }
    _release_spinlock(&ctx->lock);

    if (ctx->rm == (usize)-1) {
        printk("ctx->rm = %lld, ctx->done = %lld\n", ctx->rm, ctx->done);
        PANIC();
    }
    if (ctx->done > OP_MAX_NUM_BLOCKS) {
        PANIC();
    }

    // 如果不为空，那么会将block标记为dirty，不能被替换
}
static int write_log_count;
static int write_data_count;
void write_log() {
    // 第一步，找到所有的脏快
    Block *save[sblock->num_log_blocks - 1];
    int count = 0;
    _acquire_spinlock(&cache_lock);
    ListNode *b = head.next;
    // printk("head.addr = %p,head.next.addr = %p\n", &head, head.next);

    while (b != &head) {

        Block *block = container_of(b, Block, node);
        if (block->pinned == true) {
            // printk("log Once\n");
            save[count] = block;
            count++;
        }
        b = b->next;
    }
    write_log_count = count;
    _release_spinlock(&cache_lock);
    if ((u32)count > (sblock->inode_start - sblock->log_start) - 1) {
        // 错误检查
        PANIC();
    }

    // 第二步，将脏块写入日志
    for (int i = 0; i < count; i++) {
        Block temp;
        init_block(&temp);
        temp.block_no = sblock->log_start + 1 + i;
        for (int j = 0; j < BLOCK_SIZE; j++) {
            temp.data[j] = save[i]->data[j];
        }
        device_write(&temp);
    }
    header.valid = true;
    header.num_blocks = count;
    // printk("count = %d\n", count);
    for (int i = 0; i < count; i++) {
        // printk("block_no = %d\n", save[i]->block_no);
        header.block_no[i] = save[i]->block_no;
    }
}

void write_log_header() { write_header(); }
void write_data() {
    Block *save[sblock->num_log_blocks - 1];
    int count = 0;
    _acquire_spinlock(&cache_lock);
    ListNode *b = head.next;
    while (b != &head) {
        Block *block = container_of(b, Block, node);

        if (block->pinned == true) {
            save[count] = block;
            count++;
        }
        b = b->next;
    }
    write_data_count = count;
    if (write_data_count != write_log_count) {
        printk("write_data_count = %d,write_log_count = %d", write_data_count,
               write_log_count);
        PANIC();
    }
    _release_spinlock(&cache_lock);
    // if (count > (sblock->inode_start - sblock->log_start) - 1) {
    //     // 错误检查
    //     PANIC();
    // }
    for (int i = 0; i < count; i++) {
        device_write(save[i]);
    }
    _acquire_spinlock(&cache_lock);
    for (int i = 0; i < count; i++) {
        save[i]->pinned = false;
    }
    _release_spinlock(&cache_lock);
}
void erase_log_header() {
    init_log_header();
    write_header();
}
static void commit() {
    // 第一步，将所有的脏块写入日志
    write_log();
    // 第二步，设置日志头，最后设置valid位
    write_log_header();
    // 第三步，将日志内容写入磁盘,之后将所有的脏块标记为非脏块
    write_data();
    // 第四步，消除日志头，即把valid位设为false
    erase_log_header();
}

// see `cache.h`.
static void cache_end_op(OpContext *ctx) {
    // TODO
    // 第一步，获取日志锁
    // 第二步,错误控制，如果正在committing，那么PANIC
    // 如果outstanding的数量为0，那么就可以进行commit
    // 第三步，进行commit，之后获得日志锁，叫commiting位设为0，释放日志锁，并唤醒所有进入深睡眠的进程。
    // printk("end_op\n");
    bool can_commit = false;
    _acquire_spinlock(&log.lock);
    if (log.is_committing == true) {
        PANIC();
    }
    log.started_event_count--;
    log.operation_count -= ctx->rm;
    if (log.started_event_count == 0) {
        can_commit = true;
        log.is_committing = true;
        _release_spinlock(&log.lock);
    } else {
        post_all_sem(&log.sem);
        _lock_sem(&log.end_op_sem);
        _release_spinlock(&log.lock);
        if (_wait_sem(&log.end_op_sem, false)) {
        }
    }
    if (can_commit) {
        commit();
        _acquire_spinlock(&log.lock);
        log.is_committing = false;
        log.operation_count = 0;
        post_all_sem(&log.sem);
        post_all_sem(&log.end_op_sem);
        _release_spinlock(&log.lock);
    }
}

// see `cache.h`.
static usize cache_alloc(OpContext *ctx) {
    // TODO
    // 第一步，遍历bitmap，找到没有被使用的block
    int actual_bitmap_block_num = sblock->num_blocks / (BLOCK_SIZE * 8) + 1;
    int allocated_block_no = -1;
    bool found = false;
    for (int i = 0; i < actual_bitmap_block_num; i++) {
        if (found == true) {
            break;
        }
        int end = BLOCK_SIZE * 8;
        if (i == actual_bitmap_block_num - 1) {
            end = sblock->num_blocks % (BLOCK_SIZE * 8);
        }
        Block *block = cache_acquire(sblock->bitmap_start + i);

        for (int index = 0; index < end; index++) {
            BitmapCell *bitmap = (BitmapCell *)&block->data[0];
            if (bitmap_get(bitmap, index) == false) {
                bitmap_set((BitmapCell *)block->data, index);
                cache_sync(ctx, block);
                allocated_block_no = i * (BLOCK_SIZE * 8) + index;
                found = true;
                break;
            }
        }
        cache_release(block);
    }
    if (found == false)
        PANIC();

    // 第二步，将对应的block清空
    Block *block = cache_acquire(allocated_block_no);
    memset(block->data, 0, BLOCK_SIZE);
    cache_sync(ctx, block);
    cache_release(block);
    return allocated_block_no;
}

// see `cache.h`.
static void cache_free(OpContext *ctx, usize block_no) {
    // TODO
    // 第一步，找到对应bitmap的所在块
    int bitmap_block_no = sblock->bitmap_start + block_no / (BLOCK_SIZE * 8);
    int index = block_no % (BLOCK_SIZE * 8);
    // 第二步，将该位置0
    Block *block = cache_acquire(bitmap_block_no);
    bitmap_clear((BitmapCell *)block->data, index);
    cache_sync(ctx, block);
    cache_release(block);
}

BlockCache bcache = {
    .get_num_cached_blocks = get_num_cached_blocks,
    .acquire = cache_acquire,
    .release = cache_release,
    .begin_op = cache_begin_op,
    .sync = cache_sync,
    .end_op = cache_end_op,
    .alloc = cache_alloc,
    .free = cache_free,
};
