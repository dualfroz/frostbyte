#include "paging.h"
#include "pmm.h"
#include "string.h"

#define entryPresent 0x1
#define entryPageSize 0x80
#define addressMask 0x000FFFFFFFFFF000ull

static uint64 readCr3(void)
{
    uint64 value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static void invalidatePage(uint64 virtualAddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virtualAddr) : "memory");
}

static uint64* tableFrom(uint64 entry)
{
    return (uint64*)(entry & addressMask);
}

static uint64* nextTable(uint64* table, uint64 index, uint64 parentFlags, bool create)
{
    if (table[index] & entryPresent) {
        table[index] |= parentFlags;
        return tableFrom(table[index]);
    }

    if (!create)
        return nullptr;

    uint64 frame = pmmAlloc();
    if (frame == 0)
        return nullptr;

    memset((void*)frame, 0, frameSize);
    table[index] = frame | pagePresent | pageWrite | parentFlags;
    return (uint64*)frame;
}

bool pagingMap(uint64 virtualAddr, uint64 physicalAddr, uint64 flags)
{
    uint64 pml4Index = (virtualAddr >> 39) & 0x1FF;
    uint64 pdptIndex = (virtualAddr >> 30) & 0x1FF;
    uint64 pdIndex = (virtualAddr >> 21) & 0x1FF;
    uint64 ptIndex = (virtualAddr >> 12) & 0x1FF;
    uint64 parentFlags = flags & pageUser;

    uint64* pml4 = tableFrom(readCr3());
    uint64* pdpt = nextTable(pml4, pml4Index, parentFlags, true);
    if (!pdpt)
        return false;
    uint64* pd = nextTable(pdpt, pdptIndex, parentFlags, true);
    if (!pd)
        return false;

    if (pd[pdIndex] & entryPageSize)
        return false;

    uint64* pt = nextTable(pd, pdIndex, parentFlags, true);
    if (!pt)
        return false;

    pt[ptIndex] = (physicalAddr & addressMask) | (flags & 0xFFF) | pagePresent;
    invalidatePage(virtualAddr);
    return true;
}

uint64 pagingResolve(uint64 virtualAddr)
{
    uint64 pml4Index = (virtualAddr >> 39) & 0x1FF;
    uint64 pdptIndex = (virtualAddr >> 30) & 0x1FF;
    uint64 pdIndex = (virtualAddr >> 21) & 0x1FF;
    uint64 ptIndex = (virtualAddr >> 12) & 0x1FF;

    uint64* pml4 = tableFrom(readCr3());
    uint64* pdpt = nextTable(pml4, pml4Index, 0, false);
    if (!pdpt)
        return 0;
    uint64* pd = nextTable(pdpt, pdptIndex, 0, false);
    if (!pd)
        return 0;
    if (pd[pdIndex] & entryPageSize)
        return (pd[pdIndex] & addressMask) + (virtualAddr & 0x1FFFFF);

    uint64* pt = nextTable(pd, pdIndex, 0, false);
    if (!pt)
        return 0;
    if (!(pt[ptIndex] & entryPresent))
        return 0;
    return (pt[ptIndex] & addressMask) + (virtualAddr & 0xFFF);
}

bool pagingMapUserRange(uint64 virtualAddr, uint64 size)
{
    uint64 first = virtualAddr & ~(uint64)(frameSize - 1);
    uint64 last = (virtualAddr + size + frameSize - 1) & ~(uint64)(frameSize - 1);

    for (uint64 page = first; page < last; page += frameSize) {
        if (pagingResolve(page) != 0)
            continue;
        uint64 frame = pmmAlloc();
        if (frame == 0)
            return false;
        if (!pagingMap(page, frame, pageUser | pageWrite))
            return false;
        memset((void*)page, 0, frameSize);
    }
    return true;
}
