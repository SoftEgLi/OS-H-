#include <common/string.h>
#include <fs/inode.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
// 我的头文件
#include <kernel/console.h>
#include <kernel/sched.h>

// My Code
#define BLOCK_BASE(addr) ((u64)(addr) & ~(BLOCK_SIZE - 1))

#define DIR_PER_BLOCK (BLOCK_SIZE / sizeof(DirEntry))
/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block cache and super block to use.
            Correspondingly, you should NEVER use global instance of
            them.

    @see init_inodes
 */
static const SuperBlock *sblock;

/**
    @brief the reference to the underlying block cache.
 */
static const BlockCache *cache;

/**
    @brief global lock for inode layer.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, ref counts, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory inodes.

    We use a linked list to manage all allocated inodes.

    You can implement your own data structure if you want better performance.

    @see Inode
 */
static struct rb_root_ head;

static bool compare(rb_node lnode, rb_node rnode) {
    Inode *l = container_of(lnode, Inode, node);
    Inode *r = container_of(rnode, Inode, node);
    return l->inode_no < r->inode_no;
}

static free_inode_node free_inode_list;
static SpinLock free_inode_list_lock;
void push_free_inode(free_inode_node *node, free_inode_node *head) {
    _acquire_spinlock(&free_inode_list_lock);
    node->next = head->next;
    head->next = node;
    _release_spinlock(&free_inode_list_lock);
}
free_inode_node *pop_free_inode(free_inode_node *head) {
    _acquire_spinlock(&free_inode_list_lock);
    free_inode_node *node = head->next;
    if (node != NULL) {
        head->next = node->next;
    }
    _release_spinlock(&free_inode_list_lock);
    return node;
}

// return which block `inode_no` lives on.
static INLINE usize to_block_no(usize inode_no) {
    return sblock->inode_start + (inode_no / (INODE_PER_BLOCK));
}

// return the pointer to on-disk inode.
static INLINE InodeEntry *get_entry(Block *block, usize inode_no) {
    return ((InodeEntry *)block->data) + (inode_no % INODE_PER_BLOCK);
}

// return address array in indirect block.
static INLINE u32 *get_addrs(Block *block) {
    return ((IndirectBlock *)block->data)->addrs;
}

// initialize inode tree.
void init_inodes(const SuperBlock *_sblock, const BlockCache *_cache) {
    init_spinlock(&lock);
    init_spinlock(&free_inode_list_lock);
    // init_list_node(&head);
    sblock = _sblock;
    cache = _cache;

    if (ROOT_INODE_NO < sblock->num_inodes) {
        inodes.root = inodes.get(ROOT_INODE_NO);
    } else
        printk("(warn) init_inodes: no root inode.\n");
}

// initialize in-memory inode.
static void init_inode(Inode *inode) {
    init_sleeplock(&inode->lock);
    init_rc(&inode->rc);
    // init_list_node(&inode->node);
    inode->inode_no = 0;
    inode->valid = false;
}

// see `inode.h`.
static usize inode_alloc(OpContext *ctx, InodeType type) {
    ASSERT(type != INODE_INVALID);

    // TODO
    // 第一步，找到空闲的inode。首先从空闲的inode链表中找，如果存在，直接修改并返回，如果不存在，向下
    // 第二步，初始化该inode
    free_inode_node *free_node = pop_free_inode(&free_inode_list);
    if (free_node != NULL) {
        Block *block = cache->acquire(to_block_no(free_node->inode_no));
        InodeEntry *entry = get_entry(block, free_node->inode_no);
        entry->type = type;
        cache->sync(ctx, block);
        cache->release(block);
        return free_node->inode_no;
    }
    for (u32 i = 0; i <= sblock->num_inodes / INODE_PER_BLOCK; i++) {
        Block *block = cache->acquire(sblock->inode_start + i);
        for (u32 j = 0; j < INODE_PER_BLOCK; j++) {
            if (i == 0 && j == 0) // 保留0编号
                continue;
            InodeEntry *entry = get_entry(block, j);
            if (entry->type == INODE_INVALID) {
                // 初始化block的内容
                memset(entry, 0, sizeof(InodeEntry));
                entry->type = type;
                cache->sync(ctx, block);
                cache->release(block);
                return i * INODE_PER_BLOCK + j;
            }
        }
        cache->release(block);
    }
    // 如果没有找到，则PANIC
    PANIC();
    return 0;
}

// see `inode.h`.
static void inode_lock(Inode *inode) {
    ASSERT(inode->rc.count > 0);
    // TODO
    // 第一步，获得锁
    if (!acquire_sleeplock(&inode->lock)) {
        PANIC();
    }
    // 第二步，如果inode没有从磁盘中读出，则从磁盘中读出
    if (inode->valid == false) {
        Block *block = cache->acquire(to_block_no(inode->inode_no));
        InodeEntry *entry = get_entry(block, inode->inode_no);
        memcpy(&inode->entry, entry, sizeof(InodeEntry));
        inode->valid = true;
        cache->release(block);
    }
}

// see `inode.h`.
static void inode_unlock(Inode *inode) {
    ASSERT(inode->rc.count > 0);
    // TODO
    release_sleeplock(&inode->lock);
}

// see `inode.h`.
static void inode_sync(OpContext *ctx, Inode *inode, bool do_write) {
    // TODO
    // 没有获取inode列表锁，因为valid位的修改只会在inode元数据的修改时发生，只要获得元数据锁即可。
    // 如果是写，并且inode未从磁盘读出，PANIC
    if (do_write && inode->valid == false) {
        PANIC();
    }
    // 如果是写，并且inode已从磁盘读出，那么就将inode写回磁盘
    if (do_write && inode->valid == true) {
        Block *block = cache->acquire(to_block_no(inode->inode_no));
        InodeEntry *entry = get_entry(block, inode->inode_no);
        memcpy((void *)entry, &inode->entry, sizeof(InodeEntry));
        cache->sync(ctx, block);
        cache->release(block);
        return;
    }
    // 如果是读，并且inode未从磁盘读出，那么就读出
    if (!do_write && inode->valid == false) {

        Block *block = cache->acquire(to_block_no(inode->inode_no));
        InodeEntry *entry = get_entry(block, inode->inode_no);
        inode->entry = *entry;
        inode->valid = true;
        cache->release(block);
        return;
    }
    // 如果是读，并且inode已从磁盘读出，那么就不做任何事情
    if (!do_write && inode->valid == true) {
        return;
    }
    PANIC();
}

// see `inode.h`.
static Inode *inode_get(usize inode_no) {
    ASSERT(inode_no > 0);
    ASSERT(inode_no < sblock->num_inodes);
    _acquire_spinlock(&lock);
    Inode temp_inode = {.inode_no = inode_no};
    // TODO
    // 第一步，遍历所有的inode，找到inode_no对应的inode,将引用数加1
    // 第二步，将inode返回，如果没有找到，那么就新建一个
    // ListNode *temp = head.next;
    // while (temp != &head) {
    //     Inode *inode = container_of(temp, Inode, node);
    //     if (inode->inode_no == inode_no) {
    //         inode->rc.count++;
    //         _release_spinlock(&lock);
    //         return inode;
    //     }
    //     temp = temp->next;
    // }
    rb_node r = _rb_lookup(&temp_inode.node, &head, compare);
    if (r != NULL) {
        Inode *inode = container_of(r, Inode, node);
        _increment_rc(&inode->rc);
        _release_spinlock(&lock);
        return inode;
    }
    Inode *inode = kalloc(sizeof(Inode));
    init_inode(inode);
    inode->inode_no = inode_no;
    _increment_rc(&inode->rc);
    if (_rb_insert(&inode->node, &head, compare)) {
        PANIC();
    }
    _release_spinlock(&lock);
    return inode;
}
// see `inode.h`.
static void inode_clear(OpContext *ctx, Inode *inode) {
    // TODO

    // 第一步，释放inode对应的数据block
    InodeEntry *entry = &inode->entry;
    for (int i = 0; i < INODE_NUM_DIRECT; i++) {
        if (entry->addrs[i] != 0) {
            cache->free(ctx, entry->addrs[i]);
            entry->addrs[i] = 0;
        } else {
            break;
        }
    }
    if (entry->indirect != 0) {
        Block *indirect_block = cache->acquire(entry->indirect);
        u32 *addrs = get_addrs(indirect_block);
        for (u32 i = 0; i < INODE_NUM_INDIRECT; i++) {
            if (addrs[i] != 0) {
                cache->free(ctx, addrs[i]);
            } else {
                break;
            }
        }
        cache->release(indirect_block);
        cache->free(ctx, entry->indirect);
        entry->indirect = 0;
    }
    // 第二步，清理元数据
    entry->num_bytes = 0;
    for (u32 i = 0; i < INODE_NUM_DIRECT; i++) {
        if (inode->entry.addrs[i])
            PANIC();
    }
    if (inode->entry.indirect)
        PANIC();
    // 第三步，将inode写回磁盘
    inode_sync(ctx, inode, true);
}

// see `inode.h`.
static Inode *inode_share(Inode *inode) {
    // TODO
    _increment_rc(&inode->rc);

    return inode;
}

// see `inode.h`.
static void inode_put(OpContext *ctx, Inode *inode) {
    // TODO
    // 第一步，获得inode列表锁
    _acquire_spinlock(&lock);
    // 第二步，将计数器减一
    // 第三步，检测是否需要free掉inode

    if (inode->rc.count == 1) {
        _release_spinlock(&lock);
        inode_lock(inode);
        if (inode->entry.num_links == 0) {
            ASSERT(inode->valid == true);
            ASSERT(inode->rc.count == 1);
            inode_clear(ctx, inode);
            inode->entry.type = INODE_INVALID;
            inode_sync(ctx, inode, true);
            _acquire_spinlock(&lock);
            _rb_erase(&inode->node, &head);
            _release_spinlock(&lock);
            inode_unlock(inode);
            kfree(inode);

            free_inode_node *free_node = kalloc(sizeof(free_inode_node));
            free_node->inode_no = inode->inode_no;
            push_free_inode(free_node, &free_inode_list);
            return;
        }
        inode_unlock(inode);
        _decrement_rc(&inode->rc);

        return;
    }
    _decrement_rc(&inode->rc);

    // 第四步，释放列表锁
    _release_spinlock(&lock);
}

/**
    @brief get which block is the offset of the inode in.

    e.g. `inode_map(ctx, my_inode, 1234, &modified)` will return the block_no
    of the block that contains the 1234th byte of the file
    represented by `my_inode`.

    If a block has not been allocated for that byte, `inode_map` will
    allocate a new block and update `my_inode`, at which time, `modified`
    will be set to true.

    HOWEVER, if `ctx == NULL`, `inode_map` will NOT try to allocate any new
   block, and when it finds that the block has not been allocated, it will
   return 0.

    @param[out] modified true if some new block is allocated and `inode`
    has been changed.

    @return usize the block number of that block, or 0 if `ctx == NULL` and
    the required block has not been allocated.

    @note the caller must hold the lock of `inode`.
 */
static usize inode_map(OpContext *ctx, Inode *inode, usize offset,
                       bool *modified) {
    // TODO
    // TODO 更改inode元数据
    // 第一步，计算offset对应需要多少个block
    usize need_block_count =
        offset / BLOCK_SIZE + 1; // TODO:写需要支持大于512的写
    if (need_block_count > INODE_NUM_DIRECT + INODE_NUM_INDIRECT) {
        PANIC();
    }
    // 第二步，与已有的block数量进行比较，如果offset对应的block已经存在，那么就返回对应的block

    usize inode_block_count =
        inode->entry.num_bytes == 0
            ? 0
            : (inode->entry.num_bytes - 1) / BLOCK_SIZE + 1;
    if (inode->entry.addrs[0] && inode_block_count == 0)
        inode_block_count = 1;
    if (need_block_count <= inode_block_count) {

        *modified = false;
        if (need_block_count <= INODE_NUM_DIRECT) {
            return inode->entry.addrs[need_block_count - 1];
        } else {
            Block *indirect_block = cache->acquire(inode->entry.indirect);

            u32 *addrs = get_addrs(indirect_block);
            u32 result = addrs[need_block_count - INODE_NUM_DIRECT - 1];
            cache->release(indirect_block);
            return result;
        }
    }
    //          如果没有，则分配block，直到足够容纳下offset对应的block
    usize block_no = 0;
    if (need_block_count > inode_block_count) {

        if (ctx == NULL) {
            return 0;
        }
        *modified = true;
        if (need_block_count <= INODE_NUM_DIRECT) {
            for (u32 i = inode_block_count; i < need_block_count; i++) {
                inode->entry.addrs[i] = cache->alloc(ctx);
            }
            block_no = inode->entry.addrs[need_block_count - 1];
        }
        if (need_block_count > INODE_NUM_DIRECT) {
            if (inode_block_count <= INODE_NUM_DIRECT) {

                for (int i = inode_block_count; i < INODE_NUM_DIRECT; i++) {
                    inode->entry.addrs[i] = cache->alloc(ctx);
                }
                if (inode->entry.indirect == NULL) {
                    inode->entry.indirect = cache->alloc(ctx);
                }

                Block *indirect_block = cache->acquire(inode->entry.indirect);

                IndirectBlock *indirect_block_data =
                    (IndirectBlock *)indirect_block->data;
                for (u32 i = 0; i < need_block_count - INODE_NUM_DIRECT; i++) {
                    indirect_block_data->addrs[i] = cache->alloc(ctx);
                }

                block_no = indirect_block_data
                               ->addrs[need_block_count - INODE_NUM_DIRECT - 1];

                cache->sync(ctx, indirect_block);
                cache->release(indirect_block);
            }
            if (inode_block_count > INODE_NUM_DIRECT) {

                // ASSERT(false);
                Block *indirect_block = cache->acquire(inode->entry.indirect);
                IndirectBlock *indirect_block_data =
                    (IndirectBlock *)indirect_block->data;
                for (u32 i = inode_block_count - INODE_NUM_DIRECT;
                     i < need_block_count - INODE_NUM_DIRECT; i++) {
                    indirect_block_data->addrs[i] = cache->alloc(ctx);
                }

                block_no = indirect_block_data
                               ->addrs[need_block_count - INODE_NUM_DIRECT - 1];

                cache->sync(ctx, indirect_block);
                cache->release(indirect_block);
            }
        }
    }

    // 第三步，同步inode到磁盘
    inode_sync(ctx, inode, true);

    return block_no;
}

// see `inode.h`.
static usize inode_read(Inode *inode, u8 *dest, usize offset, usize count) {
    // 设备文件
    if (inode->entry.type == INODE_DEVICE) {
        return console_read(inode, (char *)dest, count);
    }
    InodeEntry *entry = &inode->entry;
    if (count + offset > entry->num_bytes)
        count = entry->num_bytes - offset;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= entry->num_bytes);
    ASSERT(offset <= end);
    // TODO
    ASSERT(inode->valid == true);

    usize read_size = 0;
    bool modified;

    for (usize i = offset; i < end; i = BLOCK_BASE(i + BLOCK_SIZE)) {
        Block *block = bcache.acquire(inode_map(NULL, inode, i, &modified));
        if (BLOCK_BASE(i) == BLOCK_BASE(end)) {
            // 最后一个BLOCK
            memcpy((void *)(dest + read_size), block->data + i % BLOCK_SIZE,
                   end - i);
            read_size += end - i;
        } else {
            memcpy((void *)(dest + read_size), block->data + i % BLOCK_SIZE,
                   BLOCK_SIZE - i % BLOCK_SIZE);
            read_size += BLOCK_SIZE - i % BLOCK_SIZE;
        }
        bcache.release(block);
    }
    // 1.计算跨越的block数量
    // 3.返回读出的字节
    return count;
}

// see `inode.h`.
static usize inode_write(OpContext *ctx, Inode *inode, u8 *src, usize offset,
                         usize count) {
    // 设备文件
    if (inode->entry.type == INODE_DEVICE) {
        return console_write(inode, (char *)src, count);
    }

    InodeEntry *entry = &inode->entry;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= INODE_MAX_BYTES);
    ASSERT(offset <= end);

    // TODO
    usize write_size = 0;
    bool modified;

    for (usize i = offset; i < end; i = BLOCK_BASE(i + BLOCK_SIZE)) {
        Block *block = bcache.acquire(inode_map(ctx, inode, i, &modified));
        if (BLOCK_BASE(i) == BLOCK_BASE(end)) {
            // 最后一个BLOCK
            memcpy((void *)(block->data + i % BLOCK_SIZE), src + write_size,
                   end - i);
            // printk("write successfully,count = %lld\n", end - i);
            write_size += end - i;
        } else {
            memcpy(block->data + i % BLOCK_SIZE, src + write_size,
                   BLOCK_SIZE - i % BLOCK_SIZE);
            write_size += BLOCK_SIZE - i % BLOCK_SIZE;
        }
        bcache.sync(ctx, block);
        bcache.release(block);
    }
    ASSERT(write_size == count);

    if (end > inode->entry.num_bytes) {
        inode->entry.num_bytes = end;
    }
    inode_sync(ctx, inode, true);
    return count;
    // 1.计算跨越的block数量
    // ASSERT(inode->valid == true);

    // int start_block_offset = offset / BLOCK_SIZE * BLOCK_SIZE;
    // int end_block_offset = end == 0 ? 0 : (end - 1) / BLOCK_SIZE *
    // BLOCK_SIZE; int block_count = (end_block_offset - start_block_offset) /
    // BLOCK_SIZE + 1; u32 file_offset = offset;
    // // 2.遍历block，写入需要写入的内容
    // for (int i = 0; i < block_count; i++) {
    //     int write_size, cur_offset, src_offset;
    //     if (i == 0) {
    //         cur_offset = offset % BLOCK_SIZE;
    //         write_size = end_block_offset - start_block_offset >= BLOCK_SIZE
    //                          ? BLOCK_SIZE - offset % BLOCK_SIZE
    //                          : end - offset;
    //         src_offset = 0;
    //     } else {
    //         cur_offset = 0;
    //         write_size = i == block_count - 1 ? end % BLOCK_SIZE :
    //         BLOCK_SIZE; src_offset = i * BLOCK_SIZE - offset % BLOCK_SIZE;
    //     }
    //     bool modified;
    //     usize block_no =
    //         inode_map(ctx, inode, offset + i * BLOCK_SIZE, &modified);

    //     Block *block = cache->acquire(block_no);
    //     memcpy(block->data + cur_offset, src + src_offset, write_size);
    //     file_offset += write_size;
    //     if (file_offset > entry->num_bytes) {
    //         inode->entry.num_bytes = file_offset;
    //     }
    //     cache->sync(ctx, block);
    //     cache->release(block);
    // }
    // // 3.修改inode元数据
    // inode_sync(ctx, inode, true);
    // 4.返回写回的字节
    // return count;
}

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

// see `inode.h`.
static usize inode_lookup(Inode *inode, const char *name, usize *index) {
    InodeEntry *entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    for (u64 offset = 0; offset < entry->num_bytes;
         offset += sizeof(DirEntry)) {
        DirEntry dir;
        inode_read(inode, (u8 *)&dir, offset, sizeof(DirEntry));
        if (!dir.inode_no)
            continue;
        else if (strncmp(name, dir.name, FILE_NAME_MAX_LENGTH))
            continue;
        else if (index) {
            /*经测试，此处index返回什么偏移量需要inode_remove中有对应的处理，
             *关键在于write的offset应该传字节的偏移量，remove中注意即可
             *(别的地方应该不影响)
             */
            //*index = offset / DIRENTRY_SIZE;
            *index = offset;
        }
        return dir.inode_no;
    }
    return 0;
}

// see `inode.h`.
static usize inode_insert(OpContext *ctx, Inode *inode, const char *name,
                          usize inode_no) {
    InodeEntry *entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);
    // TODO
    ASSERT(inode->valid == true);
    // 第一步，先检测是否已经存在同名文件，如果有，返回-1
    usize index;
    if (inode_lookup(inode, name, &index)) {
        return -1;
    }
    DirEntry dir_entry = {0};
    memcpy(dir_entry.name, name, strlen(name) + 1);
    dir_entry.inode_no = inode_no;
    // 如果沒有，在文件末尾进行追加写
    inode_write(ctx, inode, (u8 *)&dir_entry, inode->entry.num_bytes,
                sizeof(DirEntry));
    // 第二步，返回空闲位置的index
    index = inode->entry.num_bytes / sizeof(DirEntry) + 1;
    return index;
}

static usize inode_search_by_idx_and_set_empty(OpContext *ctx, Inode *inode,
                                               usize index) {
    ASSERT(inode->valid == true);
    ASSERT(inode->entry.type == INODE_DIRECTORY);
    int block_pos = index / DIR_PER_BLOCK;
    usize block_no;
    if (block_pos < INODE_NUM_DIRECT) {
        block_no = inode->entry.addrs[block_pos];
    } else {
        Block *indirect_block = cache->acquire(inode->entry.indirect);
        u32 *addrs = get_addrs(indirect_block);
        block_no = addrs[block_pos - INODE_NUM_DIRECT];
        cache->release(indirect_block);
    }
    Block *block = cache->acquire(block_no);
    DirEntry *dir_entry = (DirEntry *)block->data;
    usize inode_no = dir_entry[index % DIR_PER_BLOCK].inode_no;
    if (inode_no == 0) {
        cache->release(block);
        return 0;
    }
    dir_entry[index % DIR_PER_BLOCK].inode_no = 0;
    cache->sync(ctx, block);
    cache->release(block);
    return inode_no;
}

// see `inode.h`.
static void inode_remove(OpContext *ctx, Inode *inode, usize index) {
    // TODO
    ASSERT(inode->entry.type == INODE_DIRECTORY);
    ASSERT(inode->valid == true);
    // 第一步，找到对应的entry，判断该inode是否被使用，如果没有被使用，则什么都不做
    usize inode_num = inode_search_by_idx_and_set_empty(ctx, inode, index);
    if (inode_num == 0)
        return;
    // 如果被使用，则将目录对应的entry的inode_no置为0，将name置为空
    // 第二步，如果index对应的不是最后一个entry，那么就将最后一个entry移到该位置。
    // 第三步，修改num_bytes，如果修改后是BLOCK_SIZE的整数倍，那么就释放对应的块。
}

InodeTree inodes = {
    .alloc = inode_alloc,
    .lock = inode_lock,
    .unlock = inode_unlock,
    .sync = inode_sync,
    .get = inode_get,
    .clear = inode_clear,
    .share = inode_share,
    .put = inode_put,
    .read = inode_read,
    .write = inode_write,
    .lookup = inode_lookup,
    .insert = inode_insert,
    .remove = inode_remove,
};

/* LabFinal */

/**
    @brief read the next path element from `path` into `name`.

    @param[out] name next path element.

    @return const char* a pointer offseted in `path`, without leading `/`. If no
    name to remove, return NULL.

    @example
    skipelem("a/bb/c", name) = "bb/c", setting name = "a",
    skipelem("///a//bb", name) = "bb", setting name = "a",
    skipelem("a", name) = "", setting name = "a",
    skipelem("", name) = skipelem("////", name) = NULL, not setting name.
 */
static const char *skipelem(const char *path, char *name) {
    const char *s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= FILE_NAME_MAX_LENGTH)
        memmove(name, s, FILE_NAME_MAX_LENGTH);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

/**
    @brief look up and return the inode for `path`.

    If `nameiparent`, return the inode for the parent and copy the final
    path element into `name`.

    @param path a relative or absolute path. If `path` is relative, it is
    relative to the current working directory of the process.

    @param[out] name the final path element if `nameiparent` is true.

    @return Inode* the inode for `path` (or its parent if `nameiparent` is
   true), or NULL if such inode does not exist.

    @example
    namex("/a/b", false, name) = inode of b,
    namex("/a/b", true, name) = inode of a, setting name = "b",
    namex("/", true, name) = NULL (because "/" has no parent!)
 */
static Inode *namex(const char *path, bool nameiparent, char *name,
                    OpContext *ctx) {
    /* TODO: LabFinal */
    if (!strcmp(path, "/") && nameiparent) {
        return NULL;
    }
    struct proc *cur_proc = thisproc();
    bool is_absolute = path[0] == '/';
    Inode *result = NULL;
    if (is_absolute) {
        result = inodes.get(ROOT_INODE_NO);
        printk("\n");
    } else {
        ASSERT(cur_proc->cwd != NULL);
        result = cur_proc->cwd;
        inodes.share(result);
    }

    int length = strlen(path);
    int slash_count = 0;
    for (int i = 0; i < length; i++) {
        if (path[i] == '/') {
            slash_count++;
        }
    }
    char cur_name[FILE_NAME_MAX_LENGTH] = {0};
    Inode *pre_result = NULL;
    const char *rest_path = path;
    if (nameiparent) {
        slash_count -= 1;
    }
    if (!is_absolute) {
        slash_count += 1;
    }
    for (int i = 0; i < slash_count; i++) {
        rest_path = skipelem(rest_path, cur_name);
        pre_result = result;
        inodes.lock(pre_result);
        usize result_no = inodes.lookup(pre_result, cur_name, NULL);
        // printk("result_no = %lld\n", result_no);
        inodes.unlock(pre_result);
        inodes.put(ctx, pre_result);
        if (result_no == 0) {
            return NULL;
        }
        result = inodes.get(result_no);
    }
    memcpy(name, rest_path, strlen(rest_path) + 1);
    return result;
}

Inode *namei(const char *path, OpContext *ctx) {
    char name[FILE_NAME_MAX_LENGTH];
    return namex(path, false, name, ctx);
}

Inode *nameiparent(const char *path, char *name, OpContext *ctx) {
    return namex(path, true, name, ctx);
}

/**
    @brief get the stat information of `ip` into `st`.

    @note the caller must hold the lock of `ip`.
 */
void stati(Inode *ip, struct stat *st) {
    st->st_dev = 1;
    st->st_ino = ip->inode_no;
    st->st_nlink = ip->entry.num_links;
    st->st_size = ip->entry.num_bytes;
    switch (ip->entry.type) {
    case INODE_REGULAR:
        st->st_mode = S_IFREG;
        break;
    case INODE_DIRECTORY:
        st->st_mode = S_IFDIR;
        break;
    case INODE_DEVICE:
        st->st_mode = 0;
        break;
    default:
        PANIC();
    }
}