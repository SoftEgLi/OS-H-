#include "aarch64/intrinsic.h"
#include "kernel/printk.h"
#include <aarch64/mmu.h>
#include <common/checker.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/init.h>
#include <kernel/mem.h>
// My Add
#include "myData.h"
#include "myheader.h"
#include <common/string.h>

#define MY_PAGE_COUNT (PHYSTOP / PAGE_SIZE)
#define K_DEBUG 0
#define FAIL(...)                                                              \
    {                                                                          \
        printk(__VA_ARGS__);                                                   \
        while (1)                                                              \
            ;                                                                  \
    }
#define DEBUG(...)                                                             \
    { printk(__VA_ARGS__); }

SpinLock lock;
bool useTable[NUM_ENTRIES];
RefCount alloc_page_cnt;
void *shared_zero_page;
define_early_init(alloc_page_cnt) { init_rc(&alloc_page_cnt); }

static QueueNode *pages;
struct page pages_ref_array[MY_PAGE_COUNT];
extern char end[];
define_early_init(pages) {
    memset(pages_ref_array, 0, sizeof(pages_ref_array));
    // printk("pages_ref_array:addr = %llx\n", (u64)pages_ref_array);
    // printk("pages_ref_array count end = %llx\n",
    //        (u64)(&pages_ref_array[MY_PAGE_COUNT - 1].ref));
    // PANIC();
    for (u64 p = PAGE_BASE((u64)&end) + PAGE_SIZE; p < P2K(PHYSTOP);
         p += PAGE_SIZE)
        add_to_queue(&pages, (QueueNode *)p);

    // memset(shared_zero_page, 0, PAGE_SIZE);
}
define_init(init_shared_zero_page) {
    shared_zero_page = kalloc_page();
    memset(shared_zero_page, 0, PAGE_SIZE);
}

define_early_init(initTable) { init_spinlock(&lock); }

short getCeilDivEight(int size) { return (size - 1) / 8 + 1; }

void *kalloc_page() {
    _increment_rc(&alloc_page_cnt);
    if (alloc_page_cnt.count > MY_PAGE_COUNT) {
        PANIC();
    }
    // printk("My_Page_Count = %d", MY_PAGE_COUNT);
    // PANIC();
    // TODO
    u64 *addr = (u64 *)fetch_from_queue(&pages);
    // if (addr == NULL) {
    //     PANIC();
    // }
    memset((void *)addr, 0, PAGE_SIZE);
    u64 idx = (u64)K2P(addr) / PAGE_SIZE;
    _increment_rc(&(pages_ref_array[idx].ref));
    // printk("alloc_page: %llx\n", (u64)addr);
    return addr;
}

void kfree_page(void *p) {
    // _decrement_rc(&alloc_page_cnt);
    // TODO
    _decrement_rc(&(pages_ref_array[(u64)K2P(p) / PAGE_SIZE].ref));

    if (pages_ref_array[(u64)K2P(p) / PAGE_SIZE].ref.count == 0) {
        add_to_queue(&pages, (QueueNode *)p);
        _decrement_rc(&alloc_page_cnt);
    }
    // printk("free_page: %llx\n", (u64)p);
}

define_early_init(tableInit) { memset(hashTable, 0, sizeof(hashTable)); }

u64 *doAllocPage(isize size) {
    setup_checker(doAllocPage);
    // 如果没有找到，则分配一个页（原子），
    // printk("kalloc:\n");
    void *allocPageAddr = kalloc_page();
    // printk("kalloc done\n");
    // 将该页头被占用的块数设为1
    // 页地址+8处块偏移设为8+((size+sizeof(alcBlock)-1)/8+1)*8，
    short endoffset =
        sizeOfPageHeader() + getCeilDivEight(size + sizeOfOccupiedHeader()) * 8;
    if (endoffset < sizeOfFreeHeader())
        endoffset = sizeOfFreeHeader();
    BlockHeader *alcBlockHeader =
        (BlockHeader *)(allocPageAddr + sizeOfPageHeader());
    setBlockHeader(alcBlockHeader, false, endoffset, 0);
    // 后块.块偏移为4095,指针设为0
    // 如果剩余块能装下32字节，则建立free头，否则不建立。
    if (getEndOffset(alcBlockHeader) >= 4096 - sizeOfFreeHeader()) {
        setBlockEndOffset(alcBlockHeader, 4096);
        return (u64 *)(getValidSpaceAddr(alcBlockHeader));
    }
    BlockHeader *restBlockAddr =
        (BlockHeader *)(allocPageAddr + getEndOffset(alcBlockHeader));
    setBlockHeader(restBlockAddr, true, 4096, sizeOfPageHeader()); // TODO
    HashNode *freeNode = getBlockHashNode(restBlockAddr);
    int idx = getBlockSize(restBlockAddr) / 8 - 1;
    acquire_spinlock(doAllocPage, &lock);
    insertToHead(idx, freeNode);
    release_spinlock(doAllocPage, &lock);
    return (u64 *)(getValidSpaceAddr(alcBlockHeader));
    // 更新散列表信息，块大小为4096-8-((size+sizeof(alcBlock)-1)/8+1)*8,索引=块大小/8-1
}

BlockHeader *getNextBlockHeader(BlockHeader *curHeader) {
    return (BlockHeader *)(getPageAddr(curHeader) + getEndOffset(curHeader));
}

u64 *doAllocBlock(void *nodeAddr, int hashIdx, int foundIdx, isize size) {
    setup_checker(doAllocBlock);
    // 2.如果找到，则获得块地址，将该页头被占用的块数+1,
    HashNode *curNode = (HashNode *)nodeAddr; // TODO
    BlockHeader *alcBlockHeader =
        (BlockHeader *)((void *)curNode - sizeOfOccupiedHeader());
    // 更新被分配块的控制头
    short endOffset;
    bool needFreeHeader = false;
    if (hashIdx <= foundIdx - 3) {
        endOffset = getAddrOffset(alcBlockHeader) +
                    getCeilDivEight(size + sizeOfOccupiedHeader()) * 8;
        needFreeHeader = true;
    } else {
        endOffset = getAddrOffset(alcBlockHeader) +
                    getCeilDivEight(size + sizeOfOccupiedHeader()) * 8 +
                    (foundIdx - hashIdx) * 8;
    }
    short oriEndOffset = getEndOffset(alcBlockHeader);
    setBlockHeaderWithoutPre(alcBlockHeader, false, endOffset);
    // 更新free块的控制头
    if (needFreeHeader) {
        BlockHeader *restHeader = getNextBlockHeader(alcBlockHeader);
        setBlockHeader(restHeader, true, oriEndOffset,
                       getAddrOffset(alcBlockHeader));
        int idx = getBlockSize(restHeader) / 8 - 1;
        if (hasNextHeader(restHeader)) {
            BlockHeader *belowHeader = getNextBlockHeader(restHeader);
            belowHeader->preBlockOffset = getAddrOffset(restHeader);
        }
        acquire_spinlock(doAllocBlock, &lock);
        insertToHead(idx, getBlockHashNode(restHeader));
        release_spinlock(doAllocBlock, &lock);
        setNotUsed(nodeAddr);
    }
    return (u64 *)(getValidSpaceAddr(alcBlockHeader));
    // 后一个块.前块偏移=选中块地址后12位，后一个块.下一个块=hash[((后一个块.块偏移-选中块.最终偏移)-1)/8+1]
    // hash[寻找时的索引] = 修改前.8字节的下一个块
}

void *kalloc(isize size) {
    /*
        块地址：8字节对齐
        页头：被占用的块数：2字节

        被分配块的控制信息：
            2字节的块最终偏移 13bits
            1bit块状态（0表示Occupied，1表示free）
        free块的控制信息：
            2字节的块偏移   13bits
            1bit块状态
            8字节的下一个节点（在散列表中相同位置）
            8字节的前一个节点
        数据结构存放在.bss段中
    */
    setup_checker(kalloc);
    // if(K_DEBUG)
    //     printk("cpuid = %lld, begin alloc\n", cpuid());
    if (size + sizeOfOccupiedHeader() < sizeOfFreeHeader())
        size = sizeOfFreeHeader() - sizeOfOccupiedHeader();
    /*第一步，从数据结构中找到一个合适的块，如果没有，就分配页
        数据结构：hash表，函数为(size+sizeof(alcBlock)-1)/8。如果为空，则去下一个去找，直到遍历2^8+1（原子）。*/
    int hashIdx = getCeilDivEight(size + sizeOfOccupiedHeader()) - 1;
    bool found = false;
    int found_idx = 0;
    void *nodeAddr = NULL;
    acquire_spinlock(kalloc, &lock);
    for (int i = hashIdx; i < 512; i++) {
        if (hashTable[i] != NULL) {
            nodeAddr = hashTable[i];
            incPageAlcCount(nodeAddr);
            deleteHeadNode(i);
            found = true;
            found_idx = i;
            break;
        }
    }
    release_spinlock(kalloc, &lock);

    u64 *addr = 0;
    if (found == false) {
        addr = doAllocPage(size);
    } else {
        addr = doAllocBlock(nodeAddr, hashIdx, found_idx, size);
    }
    // printk("cpuid = %lld,alloc addr = %llx\n", cpuid(), (u64)addr);
    // printk("kalloc: %llx,size = %lld\n", (u64)addr, size);
    return addr;
}

void kfree(void *p) {
    // printk("kfree: %llx\n", (u64)p);

    setup_checker(kfree);
    // 第一步，根据被释放块的信息，确定endOffset，以及控制头的地址
    BlockHeader *curBlock = (BlockHeader *)(p - sizeOfOccupiedHeader());
    short endOffset = getEndOffset(curBlock);
    BlockHeader *freeHeader = curBlock;
    setBlockHeaderWithoutPre(freeHeader, true, endOffset);

    // 第三步，更新Hash表
    int idx = getBlockSize(freeHeader) / 8 - 1;
    acquire_spinlock(kfree, &lock);
    insertToHead(idx, getBlockHashNode(freeHeader));
    release_spinlock(kfree, &lock);
}
// TODO:页头的修改也要原子化

u64 left_page_cnt() { return PAGE_COUNT - alloc_page_cnt.count; }

WARN_RESULT void *get_zero_page() {
    // TODO
    // Return the shared zero page
    ASSERT((u64)shared_zero_page == 0xffff00003efff000);
    return shared_zero_page;
}