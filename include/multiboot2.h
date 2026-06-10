#ifndef FROSTBYTE_MULTIBOOT2_H
#define FROSTBYTE_MULTIBOOT2_H

#include "types.h"

#define multibootTagTypeEnd 0
#define multibootTagTypeModule 3
#define multibootTagTypeMmap 6

#define multibootMemoryAvailable 1

typedef struct MultibootTag {
    uint32 type;
    uint32 size;
} __attribute__((packed)) MultibootTag;

typedef struct MultibootTagModule {
    uint32 type;
    uint32 size;
    uint32 moduleStart;
    uint32 moduleEnd;
    char string[];
} __attribute__((packed)) MultibootTagModule;

typedef struct MultibootMmapEntry {
    uint64 baseAddr;
    uint64 length;
    uint32 type;
    uint32 reserved;
} __attribute__((packed)) MultibootMmapEntry;

typedef struct MultibootTagMmap {
    uint32 type;
    uint32 size;
    uint32 entrySize;
    uint32 entryVersion;
    MultibootMmapEntry entries[];
} __attribute__((packed)) MultibootTagMmap;

bool multibootFindModule(uint64 multibootInfo, uint64* start, uint64* end);

#endif
