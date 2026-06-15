#ifndef FROSTBYTE_INITRD_H
#define FROSTBYTE_INITRD_H

#include "types.h"

typedef struct UstarEntry {
    const char* name;
    const uint8* data;
    uint64 size;
    bool isDir;
} UstarEntry;

void initrdInit(uint64 start, uint64 end);
uint32 initrdCount(void);
bool initrdAt(uint32 index, UstarEntry* out);

#endif
