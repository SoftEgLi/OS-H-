#pragma once

#include <common/defines.h>
#include <common/spinlock.h>
#include "printk.h"
#include "myData.h"

#define EightDivHeaderSize 2

void* getPageAddr(void* addr);

typedef struct PageHeader{
    short allocCount;
}PageHeader;

void initPageHeader(void *addr);

short getPageAlcCount(void *addr);
bool isEmptyPage(void *addr);
void incPageAlcCount(void *addr);

void decPageAlcCount(void *addr);

typedef struct BlockHeader{
    short endOffsetAndStatus;
    short preBlockOffset;
    bool inUse;
} BlockHeader;

bool hasNextHeader(BlockHeader *blockHeader);
bool hasPreHeader(BlockHeader *blockHeader);
bool isFree(BlockHeader *blockHeader);
bool isUsed(BlockHeader *blockHeader);

short getEndOffset(BlockHeader *blockHeader);
short getBlockSize(BlockHeader *blockHeader);
short getAddrOffset(void* addr);
BlockHeader *getPreBlockHeader(BlockHeader *blockHeader);
HashNode *getBlockHashNode(void *blockHeader);
void *getValidSpaceAddr(void *blockHeader);

void setBlockHeaderWithoutPre(void *headerAddr, bool isFree, short endOffset);
void setBlockHeader(void *headerAddr, bool isFree, short endOffset, short preBlockOffset);
void setBlockEndOffset(BlockHeader *blockHeader, short offset);
void setFree(BlockHeader *blockHeader);
void setOccupied(BlockHeader *blockHeader);
void setUsed(void *hashNode);
void setNotUsed(void *hashNode);

int sizeOfFreeHeader();
int sizeOfOccupiedHeader();
int sizeOfPageHeader();
