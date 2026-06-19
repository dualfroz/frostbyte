#ifndef FROSTBYTE_VEC_H
#define FROSTBYTE_VEC_H

#include "types.h"

/*
 * Vec: a growable, type-erased array backed by the kernel heap. Elements
 * are stored by value (memcpy'd in and out), sized by elemSize so a single
 * implementation serves every element type without void* indirection or
 * per-type code generation.
 */

typedef struct Vec {
    uint8* data;
    usize elemSize;
    usize length;
    usize capacity;
} Vec;

void vecInit(Vec* v, usize elemSize);
void vecFree(Vec* v);

/* Ensures capacity for at least newCapacity elements. Returns false only
   if the underlying allocation failed (existing contents are untouched). */
bool vecReserve(Vec* v, usize newCapacity);

bool vecPush(Vec* v, const void* elem);
/* Removes and copies out the last element. False (and *outElem untouched)
   when the vector is empty; outElem may be nullptr to just drop it. */
bool vecPop(Vec* v, void* outElem);

/* Pointer into the backing storage; nullptr if index is out of range.
   Invalidated by any push/insert/remove/reserve that reallocates. */
void* vecAt(Vec* v, usize index);
bool vecSet(Vec* v, usize index, const void* elem);

bool vecInsert(Vec* v, usize index, const void* elem);
bool vecRemove(Vec* v, usize index, void* outElem);

void vecClear(Vec* v);

usize vecLen(const Vec* v);
usize vecCap(const Vec* v);
bool vecIsEmpty(const Vec* v);

#endif
