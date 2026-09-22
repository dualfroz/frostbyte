#ifndef FROSTBYTE_HMAP_H
#define FROSTBYTE_HMAP_H

#include "types.h"

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

uint64 hmapHashString(const char* key);

bool hmapInit(Hmap* h, usize bucketCount);
void hmapFree(Hmap* h);

bool hmapSet(Hmap* h, const char* key, void* value);
void* hmapGet(const Hmap* h, const char* key);
bool hmapHas(const Hmap* h, const char* key);
bool hmapRemove(Hmap* h, const char* key);

usize hmapCount(const Hmap* h);

void hmapForEach(const Hmap* h, bool (*visit)(const char* key, void* value, void* userData), void* userData);

#endif
