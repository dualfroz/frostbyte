#include "ktest.h"
#include "prioq.h"
#include "heap.h"

static void shuffle(uint32* arr, int32 n, uint32 seed)
{
    uint32 state = seed;
    for (int32 i = n - 1; i > 0; i--) {
        state = state * 1664525u + 1013904223u;
        int32 j = (int32)(state % (uint32)(i + 1));
        uint32 tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

KTEST(prioqEmptyQueue)
{
    PrioQueue q;
    prioqInit(&q);

    KT_TRUE(t, prioqIsEmpty(&q));
    KT_EQ(t, prioqCount(&q), 0);
    KT_NULL(t, prioqPeekMin(&q));
    KT_NULL(t, prioqPopMin(&q));
}

KTEST(prioqOrdersByVruntimeAscending)
{
    PrioQueue q;
    prioqInit(&q);

    PrioQNode nodes[5];
    uint64 vruntimes[] = { 300, 100, 500, 200, 400 };
    for (uint32 i = 0; i < 5; i++)
        prioqInsert(&q, &nodes[i], vruntimes[i], i);

    KT_EQ(t, prioqCount(&q), 5);

    uint64 expected[] = { 100, 200, 300, 400, 500 };
    for (uint32 i = 0; i < 5; i++) {
        PrioQNode* min = prioqPopMin(&q);
        KT_NOTNULL(t, min);
        if (min)
            KT_EQU(t, min->vruntime, expected[i]);
    }
    KT_TRUE(t, prioqIsEmpty(&q));
}

KTEST(prioqPeekDoesNotRemove)
{
    PrioQueue q;
    prioqInit(&q);

    PrioQNode a, b;
    prioqInsert(&q, &a, 50, 1);
    prioqInsert(&q, &b, 10, 2);

    PrioQNode* peeked = prioqPeekMin(&q);
    KT_NOTNULL(t, peeked);
    if (peeked)
        KT_EQ(t, peeked->id, 2);
    KT_EQ(t, prioqCount(&q), 2);

    PrioQNode* popped = prioqPopMin(&q);
    KT_EQP(t, popped, peeked);
    KT_EQ(t, prioqCount(&q), 1);
}

KTEST(prioqTiesBreakByIdAscending)
{
    PrioQueue q;
    prioqInit(&q);

    PrioQNode nodes[4];
    uint32 ids[] = { 7, 3, 9, 1 };
    for (uint32 i = 0; i < 4; i++)
        prioqInsert(&q, &nodes[i], 1000, ids[i]);

    uint32 expected[] = { 1, 3, 7, 9 };
    for (uint32 i = 0; i < 4; i++) {
        PrioQNode* min = prioqPopMin(&q);
        KT_NOTNULL(t, min);
        if (min)
            KT_EQ(t, min->id, expected[i]);
    }
}

KTEST(prioqRemoveArbitraryNodePreservesOrder)
{
    PrioQueue q;
    prioqInit(&q);

    PrioQNode nodes[4];
    uint64 vruntimes[] = { 10, 20, 30, 40 };
    for (uint32 i = 0; i < 4; i++)
        prioqInsert(&q, &nodes[i], vruntimes[i], i);

    prioqRemove(&q, &nodes[1]);
    KT_EQ(t, prioqCount(&q), 3);

    uint64 expected[] = { 10, 30, 40 };
    for (uint32 i = 0; i < 3; i++) {
        PrioQNode* min = prioqPopMin(&q);
        KT_NOTNULL(t, min);
        if (min)
            KT_EQU(t, min->vruntime, expected[i]);
    }
    KT_TRUE(t, prioqIsEmpty(&q));
}

KTEST(prioqNodeStorageIsReusableAcrossQueueLifetimes)
{
    PrioQNode node;

    PrioQueue first;
    prioqInit(&first);
    prioqInsert(&first, &node, 500, 42);
    PrioQNode* popped = prioqPopMin(&first);
    KT_EQP(t, popped, &node);

    PrioQueue second;
    prioqInit(&second);
    prioqInsert(&second, &node, 5, 1);
    KT_EQ(t, prioqCount(&second), 1);
    PrioQNode* min = prioqPeekMin(&second);
    KT_NOTNULL(t, min);
    if (min)
        KT_EQU(t, min->vruntime, 5);
}

#define prioqStressN 300

KTEST(prioqStressDrainsInFullyAscendingOrder)
{
    PrioQNode* pool = (PrioQNode*)kmalloc(sizeof(PrioQNode) * prioqStressN);
    uint32* order = (uint32*)kmalloc(sizeof(uint32) * prioqStressN);
    KT_NOTNULL(t, pool);
    KT_NOTNULL(t, order);
    if (!pool || !order) {
        kfree(pool);
        kfree(order);
        return;
    }

    for (uint32 i = 0; i < prioqStressN; i++)
        order[i] = i;
    shuffle(order, prioqStressN, 0xFEEDFACEu);

    PrioQueue q;
    prioqInit(&q);
    for (uint32 i = 0; i < prioqStressN; i++) {
        uint32 id = order[i];
        prioqInsert(&q, &pool[id], (uint64)id * 3, id);
    }
    KT_EQ(t, prioqCount(&q), prioqStressN);

    uint64 lastVruntime = 0;
    uint32 visited = 0;
    for (;;) {
        PrioQNode* min = prioqPopMin(&q);
        if (!min)
            break;
        if (visited > 0)
            KT_TRUE(t, min->vruntime >= lastVruntime);
        lastVruntime = min->vruntime;
        KT_EQU(t, min->vruntime, (uint64)min->id * 3);
        visited++;
    }
    KT_EQ(t, visited, prioqStressN);
    KT_TRUE(t, prioqIsEmpty(&q));

    kfree(pool);
    kfree(order);
}
