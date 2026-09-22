#ifndef FROSTBYTE_PAGING_H
#define FROSTBYTE_PAGING_H

#include "types.h"

#define pagePresent 0x1
#define pageWrite   0x2
#define pageUser    0x4

#define pageNoExecute 0x8000000000000000ull

bool pagingNxSupported(void);

bool pagingMap(uint64 virtualAddr, uint64 physicalAddr, uint64 flags);
uint64 pagingResolve(uint64 virtualAddr);

bool pagingIsExecutable(uint64 virtualAddr);

bool pagingIsWritable(uint64 virtualAddr);

bool pagingMapUserRange(uint64 virtualAddr, uint64 size);

bool pagingMapUserRangeFlags(uint64 virtualAddr, uint64 size, uint64 extraFlags);

bool pagingMapUserRangeExact(uint64 virtualAddr, uint64 size, uint64 flags);

void pagingUnmapUserRange(uint64 virtualAddr, uint64 size);

#endif
