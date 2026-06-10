#ifndef FROSTBYTE_PMM_H
#define FROSTBYTE_PMM_H

#include "types.h"

#define frameSize 4096

void pmmInit(uint64 multibootInfo, uint64 protectedEnd);
uint64 pmmAlloc(void);
void pmmFree(uint64 frame);
void pmmReserve(uint64 start, uint64 size);
uint64 pmmReservedEnd(void);
uint64 pmmTotalFrames(void);
uint64 pmmUsedFrames(void);
uint64 pmmFreeFrames(void);
uint64 pmmTotalBytes(void);

#endif
