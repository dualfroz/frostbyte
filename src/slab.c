#include "slab.h"
#include "pmm.h"

typedef struct FreeObj {
    struct FreeObj* next;
} FreeObj;

typedef struct Slab {
    struct Slab* next;
    FreeObj* freeList;
    usize freeCount;
} Slab;

static usize alignUp(usize value, usize alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static Slab* growSlab(KmemCache* cache)
{
    uint64 frame = pmmAlloc();
    if (frame == 0)
        return nullptr;

    Slab* slab = (Slab*)frame;
    slab->next = cache->slabs;
    slab->freeCount = cache->objsPerSlab;

    uint8* base = (uint8*)frame + alignUp(sizeof(Slab), sizeof(void*));
    FreeObj* prev = nullptr;
    for (usize i = 0; i < cache->objsPerSlab; i++) {
        FreeObj* obj = (FreeObj*)(base + i * cache->objSize);
        obj->next = prev;
        prev = obj;
    }
    slab->freeList = prev;

    cache->slabs = slab;
    cache->slabCount++;
    return slab;
}

bool kmemCacheInit(KmemCache* cache, const char* name, usize objSize)
{
    cache->name = name;
    cache->slabs = nullptr;
    cache->slabCount = 0;
    cache->usedObjs = 0;

    usize size = objSize < sizeof(FreeObj) ? sizeof(FreeObj) : objSize;
    size = alignUp(size, sizeof(void*));
    cache->objSize = size;

    usize headerSize = alignUp(sizeof(Slab), sizeof(void*));
    if (headerSize + size > frameSize) {
        cache->objsPerSlab = 0;
        return false;
    }
    cache->objsPerSlab = (frameSize - headerSize) / size;
    return true;
}

void kmemCacheDestroy(KmemCache* cache)
{
    Slab* slab = cache->slabs;
    while (slab != nullptr) {
        Slab* next = slab->next;
        pmmFree((uint64)slab);
        slab = next;
    }

    cache->name = nullptr;
    cache->objSize = 0;
    cache->objsPerSlab = 0;
    cache->slabs = nullptr;
    cache->slabCount = 0;
    cache->usedObjs = 0;
}

void* kmemCacheAlloc(KmemCache* cache)
{
    if (cache->objsPerSlab == 0)
        return nullptr;

    Slab* slab = cache->slabs;
    while (slab != nullptr && slab->freeList == nullptr)
        slab = slab->next;

    if (slab == nullptr) {
        slab = growSlab(cache);
        if (slab == nullptr)
            return nullptr;
    }

    FreeObj* obj = slab->freeList;
    slab->freeList = obj->next;
    slab->freeCount--;
    cache->usedObjs++;
    return (void*)obj;
}

void kmemCacheFree(KmemCache* cache, void* obj)
{
    Slab* slab = (Slab*)((uintptr)obj & ~(uintptr)(frameSize - 1));
    FreeObj* fo = (FreeObj*)obj;
    fo->next = slab->freeList;
    slab->freeList = fo;
    slab->freeCount++;
    cache->usedObjs--;
}

usize kmemCacheUsed(const KmemCache* cache)
{
    return cache->usedObjs;
}

usize kmemCacheCapacity(const KmemCache* cache)
{
    return cache->slabCount * cache->objsPerSlab;
}

usize kmemCacheSlabCount(const KmemCache* cache)
{
    return cache->slabCount;
}

usize kmemCacheObjsPerSlab(const KmemCache* cache)
{
    return cache->objsPerSlab;
}
