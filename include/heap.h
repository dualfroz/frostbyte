#ifndef FROSTBYTE_HEAP_H
#define FROSTBYTE_HEAP_H

#include "types.h"

void heapInit(uint64 start, uint64 size);
void* kmalloc(usize size);
void kfree(void* pointer);
void* krealloc(void* pointer, usize size);

uint64 heapUsed(void);
uint64 heapTotal(void);
uint64 heapFreeBytes(void);
uint64 heapLargestFree(void);
uint64 heapBlockCount(void);

#endif
