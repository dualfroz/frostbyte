#ifndef FROSTBYTE_VEC_H
#define FROSTBYTE_VEC_H

#include "types.h"

typedef struct Vec {
    uint8* data;
    usize elemSize;
    usize length;
    usize capacity;
} Vec;

void vecInit(Vec* v, usize elemSize);
void vecFree(Vec* v);

bool vecReserve(Vec* v, usize newCapacity);

bool vecPush(Vec* v, const void* elem);
bool vecPop(Vec* v, void* outElem);

void* vecAt(Vec* v, usize index);
bool vecSet(Vec* v, usize index, const void* elem);

bool vecInsert(Vec* v, usize index, const void* elem);
bool vecRemove(Vec* v, usize index, void* outElem);

void vecClear(Vec* v);

usize vecLen(const Vec* v);
usize vecCap(const Vec* v);
bool vecIsEmpty(const Vec* v);

#endif
