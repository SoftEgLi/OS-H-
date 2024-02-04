#pragma once

#include <common/defines.h>
#include <common/spinlock.h>
#define NUM_ENTRIES 512

typedef struct HashNode{
    struct HashNode *prev,*next;
} HashNode;


HashNode *hashTable[NUM_ENTRIES];

HashNode *getNodeWithIdx(int);

HashNode *deleteHeadNode(int);

HashNode *deleteNode(HashNode *hashNode,int idx);

void insertToHead(int, HashNode *hashNode);

