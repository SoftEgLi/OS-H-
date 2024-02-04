#include "myheader.h"
#define K_DEBUG 0

#define FAIL(...)            \
    {                        \
        printk(__VA_ARGS__); \
        while (1)            \
            ;                \
    }
void *getPageAddr(void *addr)
{
    void* temp = (void*)((u64)addr & 0xfffffffffffff000);
    return temp;
}
void initPageHeader(void* addr){
    PageHeader *pageHeader = (PageHeader *)addr;
    pageHeader->allocCount = 0;
}
bool hasNextHeader(BlockHeader *blockHeader){
    if(getEndOffset(blockHeader) == 0x1000)
        return false;
    return true;
}
bool hasPreHeader(BlockHeader *blockHeader){
    if(blockHeader->preBlockOffset == 0)
        return false;
    return true;
}


bool isFree(BlockHeader* blockHeader){
    if ((blockHeader->endOffsetAndStatus & 0x2000) > 0)
    {
        return 1;
    }
    else
        return 0;
}

bool isUsed(BlockHeader* blockHeader){
    return blockHeader->inUse;
}

BlockHeader* getPreBlockHeader(BlockHeader* blockHeader){
    return (BlockHeader *)(getPageAddr(blockHeader) + blockHeader->preBlockOffset);
}

short getEndOffset(BlockHeader* blockHeader){
    short offset = blockHeader->endOffsetAndStatus & 0x1fff;
    return offset;
}
short getBlockSize(BlockHeader* blockHeader){
    short size = getEndOffset(blockHeader) - getAddrOffset(blockHeader);
    return size;
}
short getAddrOffset(void* addr){
    short offset = (short)((u64)addr & 0x0000000000000fff);
    return offset;
}
HashNode* getBlockHashNode(void* blockHeader){
    return (HashNode *)(blockHeader + (sizeof(BlockHeader) + 7) / 8 * 8);
}

void* getValidSpaceAddr(void* blockHeader){
    return (void *)(blockHeader + (sizeof(BlockHeader) + 7) / 8 * 8);
}

void setBlockHeaderWithoutPre(void* headerAddr,bool isFree,short endOffset){
    BlockHeader *blockHeader = (BlockHeader *)headerAddr;
    blockHeader->endOffsetAndStatus = endOffset;
    if(isFree)
        setFree(blockHeader);
    else
        setOccupied(blockHeader);
    blockHeader->inUse = 0;
    if (K_DEBUG)
    {
        if((blockHeader->endOffsetAndStatus & 0x1fff) > 0x1000)
            FAIL("Offset overflow");
    }
}

void setBlockHeader(void* headerAddr,bool isFree,short endOffset,short preBlockOffset){
    //第一步，设置头
    //  头地址为curNode-sizeof(BlockHeader)，endoffset为头地址后12位+size除以8向上取整
    BlockHeader *blockHeader = (BlockHeader *)headerAddr;
    blockHeader->endOffsetAndStatus = endOffset;
    if(isFree)
        setFree(blockHeader);
    else
        setOccupied(blockHeader);
    blockHeader->preBlockOffset = preBlockOffset;
    blockHeader->inUse = 0;
    if (K_DEBUG)
    {
        if((blockHeader->endOffsetAndStatus & 0x1fff) > 0x1000)
            FAIL("Offset overflow");
    }
}

void setBlockEndOffset(BlockHeader* blockHeader,short offset){
    short status = blockHeader->endOffsetAndStatus & 0xe000;
    short result = offset | status;
    blockHeader->endOffsetAndStatus = result;
}

void setFree(BlockHeader* blockHeader){
    blockHeader->endOffsetAndStatus = blockHeader->endOffsetAndStatus | 0x2000;
}

void setOccupied(BlockHeader* blockHeader){
    blockHeader->endOffsetAndStatus = blockHeader->endOffsetAndStatus & 0x1fff;
}

void setUsed(void *hashNode){
    BlockHeader *blockHeader = (BlockHeader *)(hashNode - sizeOfOccupiedHeader());
    blockHeader->inUse = 1;
}
void setNotUsed(void *hashNode){
    BlockHeader *blockHeader = (BlockHeader *)(hashNode - sizeOfOccupiedHeader());
    blockHeader->inUse = 0;
}

int sizeOfFreeHeader(){
    return (sizeof(BlockHeader) + 7) / 8 * 8 + sizeof(HashNode);
}

int sizeOfOccupiedHeader(){
    return (sizeof(BlockHeader) + 7) / 8 * 8;
}
int sizeOfPageHeader(){
    return (sizeof(PageHeader) + 7) / 8 * 8;
}

short getPageAlcCount(void* addr){
    PageHeader *pageHeader = (PageHeader*)((u64)addr & 0xfffffffffffff000);
    return pageHeader->allocCount;
}
bool isEmptyPage(void* addr){
    if(getPageAlcCount(addr) == 0){
        return true;
    }
    return false;
}

void incPageAlcCount(void* addr){
    // setup_checker(incPage);
    PageHeader *pageHeader = (PageHeader*)((u64)addr & 0xfffffffffffff000);
    pageHeader->allocCount = pageHeader->allocCount + 1;
}

void decPageAlcCount(void* addr){
    PageHeader *pageHeader = (PageHeader*)((u64)addr & 0xfffffffffffff000);
    pageHeader->allocCount = pageHeader->allocCount - 1;
}
