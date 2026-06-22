#include "bmp.h"

#define bmpBitsPerWord 64

static usize wordCountFor(usize bitCount)
{
    return (bitCount + bmpBitsPerWord - 1) / bmpBitsPerWord;
}

void bmpInit(Bitmap* b, uint64* backingWords, usize bitCount)
{
    b->words = backingWords;
    b->bitCount = bitCount;
    bmpClearAll(b);
}

void bmpSet(Bitmap* b, usize index)
{
    if (index >= b->bitCount)
        return;
    b->words[index / bmpBitsPerWord] |= (1UL << (index % bmpBitsPerWord));
}

void bmpClear(Bitmap* b, usize index)
{
    if (index >= b->bitCount)
        return;
    b->words[index / bmpBitsPerWord] &= ~(1UL << (index % bmpBitsPerWord));
}

bool bmpTest(const Bitmap* b, usize index)
{
    if (index >= b->bitCount)
        return false;
    return (b->words[index / bmpBitsPerWord] & (1UL << (index % bmpBitsPerWord))) != 0;
}

void bmpSetRange(Bitmap* b, usize start, usize count)
{
    for (usize i = start; i < start + count && i < b->bitCount; i++)
        bmpSet(b, i);
}

void bmpClearRange(Bitmap* b, usize start, usize count)
{
    for (usize i = start; i < start + count && i < b->bitCount; i++)
        bmpClear(b, i);
}

void bmpSetAll(Bitmap* b)
{
    usize words = wordCountFor(b->bitCount);
    for (usize i = 0; i < words; i++)
        b->words[i] = ~0UL;
}

void bmpClearAll(Bitmap* b)
{
    usize words = wordCountFor(b->bitCount);
    for (usize i = 0; i < words; i++)
        b->words[i] = 0;
}

usize bmpCountSet(const Bitmap* b)
{
    usize total = 0;
    for (usize i = 0; i < b->bitCount; i++) {
        if (bmpTest(b, i))
            total++;
    }
    return total;
}

isize bmpFindFirstClear(const Bitmap* b)
{
    for (usize i = 0; i < b->bitCount; i++) {
        if (!bmpTest(b, i))
            return (isize)i;
    }
    return -1;
}

isize bmpFindFirstSet(const Bitmap* b)
{
    for (usize i = 0; i < b->bitCount; i++) {
        if (bmpTest(b, i))
            return (isize)i;
    }
    return -1;
}
