#include "ktest.h"
#include "slab.h"
#include "pmm.h"
#include "string.h"
#include "heap.h"

KTEST(slabInitRejectsOversizedObject)
{
    KmemCache cache;
    KT_FALSE(t, kmemCacheInit(&cache, "toobig", frameSize));
    KT_EQ(t, kmemCacheCapacity(&cache), 0);
    KT_NULL(t, kmemCacheAlloc(&cache));
}

KTEST(slabAllocDistinctNonOverlapping)
{
    KmemCache cache;
    KT_TRUE(t, kmemCacheInit(&cache, "small32", 32));
    KT_GT(t, kmemCacheObjsPerSlab(&cache), 0);
    KT_EQ(t, kmemCacheCapacity(&cache), 0);   /* nothing grown yet */

    void* a = kmemCacheAlloc(&cache);
    void* b = kmemCacheAlloc(&cache);
    void* c = kmemCacheAlloc(&cache);
    KT_NOTNULL(t, a);
    KT_NOTNULL(t, b);
    KT_NOTNULL(t, c);
    KT_NE(t, a, b);
    KT_NE(t, b, c);
    KT_NE(t, a, c);
    KT_EQ(t, kmemCacheUsed(&cache), 3);

    /* write a distinct pattern into each object and check none of them
       stomped on a neighbor */
    memset(a, 0xAA, 32);
    memset(b, 0xBB, 32);
    memset(c, 0xCC, 32);
    uint8 probe[32];
    memset(probe, 0xAA, 32);
    KT_EQ(t, memcmp(a, probe, 32), 0);
    memset(probe, 0xBB, 32);
    KT_EQ(t, memcmp(b, probe, 32), 0);
    memset(probe, 0xCC, 32);
    KT_EQ(t, memcmp(c, probe, 32), 0);

    kmemCacheFree(&cache, a);
    kmemCacheFree(&cache, b);
    kmemCacheFree(&cache, c);
    KT_EQ(t, kmemCacheUsed(&cache), 0);

    kmemCacheDestroy(&cache);
}

KTEST(slabFreeReusesLifo)
{
    KmemCache cache;
    kmemCacheInit(&cache, "reuse", 16);

    void* a = kmemCacheAlloc(&cache);
    kmemCacheFree(&cache, a);
    void* b = kmemCacheAlloc(&cache);
    KT_EQP(t, a, b);   /* single-slot free list, must come back the same */

    kmemCacheFree(&cache, b);
    kmemCacheDestroy(&cache);
}

KTEST(slabGrowsAcrossMultipleSlabs)
{
    KmemCache cache;
    kmemCacheInit(&cache, "grow64", 64);
    usize perSlab = kmemCacheObjsPerSlab(&cache);
    KT_GT(t, perSlab, 0);

    uint64 framesBefore = pmmFreeFrames();

    /* fill exactly the first slab */
    void* first = nullptr;
    for (usize i = 0; i < perSlab; i++) {
        void* p = kmemCacheAlloc(&cache);
        KT_NOTNULL(t, p);
        if (i == 0)
            first = p;
    }
    KT_EQ(t, kmemCacheSlabCount(&cache), 1);
    KT_EQ(t, kmemCacheUsed(&cache), perSlab);

    /* one more forces a second slab (a second frame from pmm) */
    void* overflow = kmemCacheAlloc(&cache);
    KT_NOTNULL(t, overflow);
    KT_EQ(t, kmemCacheSlabCount(&cache), 2);
    KT_EQ(t, kmemCacheCapacity(&cache), perSlab * 2);
    KT_EQ(t, pmmFreeFrames(), framesBefore - 2);
    KT_NE(t, overflow, first);

    kmemCacheDestroy(&cache);
    /* destroying the cache must return every frame it ever grew into */
    KT_EQ(t, pmmFreeFrames(), framesBefore);
}

#define slabStressCount 800

KTEST(slabStressAllocFreePattern)
{
    KmemCache cache;
    kmemCacheInit(&cache, "stress", 40);

    void** slots = (void**)kmalloc(sizeof(void*) * slabStressCount);
    KT_NOTNULL(t, slots);
    if (!slots) {
        kmemCacheDestroy(&cache);
        return;
    }

    for (int i = 0; i < slabStressCount; i++) {
        slots[i] = kmemCacheAlloc(&cache);
        KT_NOTNULL(t, slots[i]);
        if (slots[i])
            *(int32*)slots[i] = i;
    }
    KT_EQ(t, kmemCacheUsed(&cache), slabStressCount);

    /* every slot still holds the value we stamped, none aliased */
    for (int i = 0; i < slabStressCount; i++) {
        if (slots[i])
            KT_EQ(t, *(int32*)slots[i], i);
    }

    /* free every third object, then re-allocate that many and confirm the
       cache handed back exactly that many reclaimed slots (no capacity
       growth needed to satisfy them) */
    usize freedCount = 0;
    for (int i = 0; i < slabStressCount; i += 3) {
        kmemCacheFree(&cache, slots[i]);
        slots[i] = nullptr;
        freedCount++;
    }
    KT_EQ(t, kmemCacheUsed(&cache), slabStressCount - freedCount);

    usize capacityBeforeRefill = kmemCacheCapacity(&cache);
    for (int i = 0; i < slabStressCount; i += 3) {
        slots[i] = kmemCacheAlloc(&cache);
        KT_NOTNULL(t, slots[i]);
    }
    KT_EQ(t, kmemCacheCapacity(&cache), capacityBeforeRefill);
    KT_EQ(t, kmemCacheUsed(&cache), slabStressCount);

    for (int i = 0; i < slabStressCount; i++)
        kmemCacheFree(&cache, slots[i]);
    KT_EQ(t, kmemCacheUsed(&cache), 0);

    kfree(slots);
    kmemCacheDestroy(&cache);
}
