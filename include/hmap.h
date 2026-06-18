#ifndef FROSTBYTE_HMAP_H
#define FROSTBYTE_HMAP_H

#include "types.h"

/*
 * Hmap: a string-keyed hash map with chained buckets, backed by the kernel
 * heap. Keys are copied into owned storage on hmapSet (the caller's key
 * buffer need not outlive the call); values are stored as opaque void*
 * (the map does not take ownership of pointed-to value memory).
 */

typedef struct HmapEntry {
    char* key;
    void* value;
    struct HmapEntry* next;
} HmapEntry;

typedef struct Hmap {
    HmapEntry** buckets;
    usize bucketCount;
    usize count;
} Hmap;

/* FNV-1a. Exposed so tests (and callers who want a custom bucket count
   heuristic) can reason about it directly. */
uint64 hmapHashString(const char* key);

bool hmapInit(Hmap* h, usize bucketCount);
void hmapFree(Hmap* h);

/* Inserts key/value, or overwrites value if key is already present.
   Returns false only on allocation failure. */
bool hmapSet(Hmap* h, const char* key, void* value);
void* hmapGet(const Hmap* h, const char* key);
bool hmapHas(const Hmap* h, const char* key);
bool hmapRemove(Hmap* h, const char* key);

usize hmapCount(const Hmap* h);

/* Visits every entry in unspecified order; stop early if visit returns
   false. userData is passed through unchanged. */
void hmapForEach(const Hmap* h, bool (*visit)(const char* key, void* value, void* userData), void* userData);

#endif
