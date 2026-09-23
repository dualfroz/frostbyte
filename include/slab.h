#ifndef FROSTBYTE_SLAB_H
#define FROSTBYTE_SLAB_H

#include "types.h"

typedef struct KmemCache {
    const char* name;
    usize objSize;
    usize objsPerSlab;
    struct Slab* slabs;
    usize slabCount;
    usize usedObjs;
} KmemCache;

bool kmemCacheInit(KmemCache* cache, const char* name, usize objSize);

void kmemCacheDestroy(KmemCache* cache);

void* kmemCacheAlloc(KmemCache* cache);

void kmemCacheFree(KmemCache* cache, void* obj);

usize kmemCacheUsed(const KmemCache* cache);
usize kmemCacheCapacity(const KmemCache* cache);
usize kmemCacheSlabCount(const KmemCache* cache);
usize kmemCacheObjsPerSlab(const KmemCache* cache);

#endif
