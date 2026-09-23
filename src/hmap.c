#include "hmap.h"
#include "heap.h"
#include "string.h"
#include "klib.h"

uint64 hmapHashString(const char* key)
{
    uint64 hash = 1469598103934665603UL;
    for (const unsigned char* p = (const unsigned char*)key; *p; p++) {
        hash ^= (uint64)*p;
        hash *= 1099511628211UL;
    }
    return hash;
}

bool hmapInit(Hmap* h, usize bucketCount)
{
    if (bucketCount == 0)
        bucketCount = 16;

    h->buckets = (HmapEntry**)kmalloc(bucketCount * sizeof(HmapEntry*));
    if (!h->buckets)
        return false;

    for (usize i = 0; i < bucketCount; i++)
        h->buckets[i] = nullptr;
    h->bucketCount = bucketCount;
    h->count = 0;
    return true;
}

void hmapFree(Hmap* h)
{
    for (usize i = 0; i < h->bucketCount; i++) {
        HmapEntry* entry = h->buckets[i];
        while (entry) {
            HmapEntry* next = entry->next;
            kfree(entry->key);
            kfree(entry);
            entry = next;
        }
    }
    kfree(h->buckets);
    h->buckets = nullptr;
    h->bucketCount = 0;
    h->count = 0;
}

static HmapEntry** findSlot(const Hmap* h, const char* key)
{
    usize bucket = hmapHashString(key) % h->bucketCount;
    HmapEntry** slot = &h->buckets[bucket];
    while (*slot && strcmp((*slot)->key, key) != 0)
        slot = &(*slot)->next;
    return slot;
}

bool hmapSet(Hmap* h, const char* key, void* value)
{
    HmapEntry** slot = findSlot(h, key);
    if (*slot) {
        (*slot)->value = value;
        return true;
    }

    HmapEntry* entry = (HmapEntry*)kmalloc(sizeof(HmapEntry));
    if (!entry)
        return false;

    usize keyLen = strlen(key);
    entry->key = (char*)kmalloc(keyLen + 1);
    if (!entry->key) {
        kfree(entry);
        return false;
    }
    strcpy(entry->key, key);
    entry->value = value;
    entry->next = nullptr;

    *slot = entry;
    h->count++;
    return true;
}

void* hmapGet(const Hmap* h, const char* key)
{
    HmapEntry** slot = findSlot(h, key);
    return *slot ? (*slot)->value : nullptr;
}

bool hmapHas(const Hmap* h, const char* key)
{
    return *findSlot(h, key) != nullptr;
}

bool hmapRemove(Hmap* h, const char* key)
{
    HmapEntry** slot = findSlot(h, key);
    if (!*slot)
        return false;

    HmapEntry* entry = *slot;
    *slot = entry->next;
    kfree(entry->key);
    kfree(entry);
    h->count--;
    return true;
}

usize hmapCount(const Hmap* h)
{
    return h->count;
}

void hmapForEach(const Hmap* h, bool (*visit)(const char* key, void* value, void* userData), void* userData)
{
    for (usize i = 0; i < h->bucketCount; i++) {
        for (HmapEntry* entry = h->buckets[i]; entry; entry = entry->next) {
            if (!visit(entry->key, entry->value, userData))
                return;
        }
    }
}
