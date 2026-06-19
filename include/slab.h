#ifndef FROSTBYTE_SLAB_H
#define FROSTBYTE_SLAB_H

#include "types.h"

/*
 * Slab: a fixed-size object cache backed directly by physical frames from
 * pmm, bypassing the general-purpose heap. Each slab is exactly one frame;
 * the Slab descriptor lives at the start of that same frame (so freeing a
 * pointer is O(1): mask it down to the enclosing frame to recover the
 * descriptor, no list walk needed), and the rest of the frame is carved
 * into objSize-aligned objects threaded onto a singly-linked free list
 * through their own (currently-unused) storage.
 *
 * Intended for kernel-internal fixed-size allocations (list/tree nodes,
 * PCB-sized structs, ...) where slab's O(1) alloc/free and per-slab
 * locality beat the heap's general-purpose free-list search.
 */

typedef struct KmemCache {
    const char* name;
    usize objSize;         /* rounded up to at least sizeof(void*), 8-aligned */
    usize objsPerSlab;
    struct Slab* slabs;    /* every slab owned by this cache, newest first */
    usize slabCount;
    usize usedObjs;
} KmemCache;

/* Returns false if objSize (after rounding) cannot fit at least one object
   per frame alongside the slab header -- the cache is left zeroed either
   way, so kmemCacheDestroy is always safe to call. */
bool kmemCacheInit(KmemCache* cache, const char* name, usize objSize);

/* Releases every frame the cache ever grew into back to pmm. The cache is
   left in a freshly-initialized (empty, growable-again) state. */
void kmemCacheDestroy(KmemCache* cache);

/* Grows the cache (allocates a new frame from pmm) on demand. nullptr only
   if pmm is out of frames. */
void* kmemCacheAlloc(KmemCache* cache);

/* obj must have come from kmemCacheAlloc on this exact cache and not
   already be free. */
void kmemCacheFree(KmemCache* cache, void* obj);

usize kmemCacheUsed(const KmemCache* cache);
/* Objects across every slab the cache has grown into so far -- 0 until the
   first kmemCacheAlloc, since slabs are only ever grown lazily on demand. */
usize kmemCacheCapacity(const KmemCache* cache);
usize kmemCacheSlabCount(const KmemCache* cache);
/* How many objects a single slab (frame) holds for this cache -- fixed at
   kmemCacheInit time, independent of how many slabs have been grown. */
usize kmemCacheObjsPerSlab(const KmemCache* cache);

#endif
