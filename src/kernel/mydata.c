#include "myData.h"
#include "printk.h"
HashNode* getNodeWithIdx(int idx){
    return hashTable[idx];
}
extern SpinLock lock;
HashNode *deleteHeadNode(int foundIdx)
{
    HashNode *target = hashTable[foundIdx];
    if(target->next!=NULL)
        target->next->prev = NULL;
    hashTable[foundIdx] = target->next;
    target->next = NULL;
    return target;
}
HashNode *deleteNode(HashNode *hashNode,int idx){
    if (hashNode->prev == NULL)
    {
        hashTable[idx] = hashNode->next;
    }
    else
    {
        hashNode->prev->next = hashNode->next;
    }
    if(hashNode->next != NULL){
        hashNode->next->prev = hashNode->prev;
    }
    return hashNode;
}

void insertToHead(int idx,HashNode* hashNode){
    hashNode->prev = NULL;
    hashNode->next = hashTable[idx];
    if(hashTable[idx]!=NULL){
        hashTable[idx]->prev = hashNode;
    }
    hashTable[idx] = hashNode;
}