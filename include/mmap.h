#ifndef FROSTBYTE_MMAP_H
#define FROSTBYTE_MMAP_H

#include "types.h"
#include "fs.h"

struct Thread;

#define maxVmasPerThread 8

typedef struct Vma {
    bool used;
    uint64 start;
    uint64 end;
    uint32 prot;
    bool anonymous;
    int fd;
    uint64 fileOffset;
} Vma;

bool mmapReserve(struct Thread* thread, uint64 addr, uint64 length, uint32 prot,
                  bool anonymous, int fd, uint64 fileOffset, uint64* outAddr);

bool mmapRelease(struct Thread* thread, uint64 addr, uint64 length);

Vma* mmapFind(struct Thread* thread, uint64 addr);

bool mmapFaultHandler(struct Thread* thread, uint64 faultAddr, bool forWrite);

void mmapReleaseAll(struct Thread* thread);

#endif
