#ifndef FROSTBYTE_BMP_H
#define FROSTBYTE_BMP_H

#include "types.h"

typedef struct Bitmap {
    uint64* words;
    usize bitCount;
} Bitmap;

void bmpInit(Bitmap* b, uint64* backingWords, usize bitCount);

void bmpSet(Bitmap* b, usize index);
void bmpClear(Bitmap* b, usize index);
bool bmpTest(const Bitmap* b, usize index);

void bmpSetRange(Bitmap* b, usize start, usize count);
void bmpClearRange(Bitmap* b, usize start, usize count);

void bmpSetAll(Bitmap* b);
void bmpClearAll(Bitmap* b);

usize bmpCountSet(const Bitmap* b);

isize bmpFindFirstClear(const Bitmap* b);
isize bmpFindFirstSet(const Bitmap* b);

#endif
