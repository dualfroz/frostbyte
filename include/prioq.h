#ifndef FROSTBYTE_PRIOQ_H
#define FROSTBYTE_PRIOQ_H

#include "types.h"
#include "rbt.h"

typedef struct PrioQNode {
    RbtNode node;
    uint64 vruntime;
    uint32 id;
} PrioQNode;

typedef struct PrioQueue {
    RbTree tree;
} PrioQueue;

void prioqInit(PrioQueue* q);

void prioqInsert(PrioQueue* q, PrioQNode* n, uint64 vruntime, uint32 id);

PrioQNode* prioqPeekMin(const PrioQueue* q);

PrioQNode* prioqPopMin(PrioQueue* q);

void prioqRemove(PrioQueue* q, PrioQNode* n);

usize prioqCount(const PrioQueue* q);
bool prioqIsEmpty(const PrioQueue* q);

#endif
