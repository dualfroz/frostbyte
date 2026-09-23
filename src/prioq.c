#include "prioq.h"

static int compareNodes(const RbtNode* a, const RbtNode* b)
{
    const PrioQNode* pa = rbtEntry(a, PrioQNode, node);
    const PrioQNode* pb = rbtEntry(b, PrioQNode, node);

    if (pa->vruntime != pb->vruntime)
        return pa->vruntime < pb->vruntime ? -1 : 1;
    if (pa->id != pb->id)
        return pa->id < pb->id ? -1 : 1;
    return 0;
}

void prioqInit(PrioQueue* q)
{
    rbtInit(&q->tree);
}

void prioqInsert(PrioQueue* q, PrioQNode* n, uint64 vruntime, uint32 id)
{
    n->vruntime = vruntime;
    n->id = id;
    bool inserted = rbtInsert(&q->tree, &n->node, compareNodes);
    (void)inserted;
}

PrioQNode* prioqPeekMin(const PrioQueue* q)
{
    RbtNode* n = rbtFirst(&q->tree);
    return n ? rbtEntry(n, PrioQNode, node) : nullptr;
}

PrioQNode* prioqPopMin(PrioQueue* q)
{
    PrioQNode* min = prioqPeekMin(q);
    if (min)
        rbtRemove(&q->tree, &min->node);
    return min;
}

void prioqRemove(PrioQueue* q, PrioQNode* n)
{
    rbtRemove(&q->tree, &n->node);
}

usize prioqCount(const PrioQueue* q)
{
    return rbtCount(&q->tree);
}

bool prioqIsEmpty(const PrioQueue* q)
{
    return rbtIsEmpty(&q->tree);
}
