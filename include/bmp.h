#ifndef FROSTBYTE_BMP_H
#define FROSTBYTE_BMP_H

#include "types.h"

/*
 * Bitmap: general-purpose fixed-size bitmap over caller-supplied 64-bit
 * word storage. No heap dependency (the pmm frame allocator uses its own
 * private copy of this same idea and predates this generic version; this
 * one is for any other subsystem that needs a bit-indexed set).
 */

typedef struct Bitmap {
    uint64* words;
    usize bitCount;
} Bitmap;

/* backingWords must hold at least (bitCount + 63) / 64 uint64 words. */
void bmpInit(Bitmap* b, uint64* backingWords, usize bitCount);

void bmpSet(Bitmap* b, usize index);
void bmpClear(Bitmap* b, usize index);
bool bmpTest(const Bitmap* b, usize index);

void bmpSetRange(Bitmap* b, usize start, usize count);
void bmpClearRange(Bitmap* b, usize start, usize count);

void bmpSetAll(Bitmap* b);
void bmpClearAll(Bitmap* b);

usize bmpCountSet(const Bitmap* b);

/* Returns the index of the first clear/set bit, or -1 if none exists. */
isize bmpFindFirstClear(const Bitmap* b);
isize bmpFindFirstSet(const Bitmap* b);

#endif
