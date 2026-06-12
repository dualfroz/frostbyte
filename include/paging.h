#ifndef FROSTBYTE_PAGING_H
#define FROSTBYTE_PAGING_H

#include "types.h"

#define pagePresent 0x1
#define pageWrite   0x2
#define pageUser    0x4

bool pagingMap(uint64 virtualAddr, uint64 physicalAddr, uint64 flags);
uint64 pagingResolve(uint64 virtualAddr);
bool pagingMapUserRange(uint64 virtualAddr, uint64 size);

#endif
