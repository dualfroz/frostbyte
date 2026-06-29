#include "paging.h"
#include "pmm.h"
#include "string.h"

#define entryPresent 0x1
#define entryPageSize 0x80
#define addressMask 0x000FFFFFFFFFF000ull

/* Set by boot.S once, before kmain ever runs: 1 if CPUID.80000001H:EDX.NX
   was reported AND IA32_EFER.NXE was actually written, 0 otherwise (either
   the CPU never claimed the feature, or -- unreachable on any CPU that
   passed this kernel's own checkLongMode, but checked independently and
   honestly anyway -- some hypervisor quirk refused it). A plain uint32,
   not this codebase's own enum bool, so boot.S's 32-bit `movl` store has
   an unambiguous, non-implementation-defined width to write into. */
extern uint32 cpuNxSupported;

bool pagingNxSupported(void)
{
    return cpuNxSupported != 0;
}

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

    /* flags & 0xFFF keeps the low-12-bits PTE flags (pagePresent/pageWrite/
       pageUser, plus whatever hardware-ignored bits a caller might pass);
       pageNoExecute (bit 63) is a completely separate mask so it survives
       that truncation -- silently dropped rather than written when
       pagingNxSupported() is false, since bit 63 of a PTE is architecturally
       reserved (must be 0) whenever IA32_EFER.NXE is clear, and setting a
       reserved bit faults. */
    uint64 leafFlags = (flags & 0xFFF) | pagePresent;
    if ((flags & pageNoExecute) && pagingNxSupported())
        leafFlags |= pageNoExecute;

    pt[ptIndex] = (physicalAddr & addressMask) | leafFlags;
    invalidatePage(virtualAddr);
    return true;
}

bool pagingIsExecutable(uint64 virtualAddr)
{
    uint64 pml4Index = (virtualAddr >> 39) & 0x1FF;
    uint64 pdptIndex = (virtualAddr >> 30) & 0x1FF;
    uint64 pdIndex = (virtualAddr >> 21) & 0x1FF;
    uint64 ptIndex = (virtualAddr >> 12) & 0x1FF;

    uint64* pml4 = tableFrom(readCr3());
    uint64* pdpt = nextTable(pml4, pml4Index, 0, false);
    if (!pdpt)
        return false;
    uint64* pd = nextTable(pdpt, pdptIndex, 0, false);
    if (!pd)
        return false;
    if (pd[pdIndex] & entryPageSize)
        return (pd[pdIndex] & entryPresent) && !(pd[pdIndex] & pageNoExecute);

    uint64* pt = nextTable(pd, pdIndex, 0, false);
    if (!pt)
        return false;
    if (!(pt[ptIndex] & entryPresent))
        return false;
    return !(pt[ptIndex] & pageNoExecute);
}

bool pagingIsWritable(uint64 virtualAddr)
{
    uint64 pml4Index = (virtualAddr >> 39) & 0x1FF;
    uint64 pdptIndex = (virtualAddr >> 30) & 0x1FF;
    uint64 pdIndex = (virtualAddr >> 21) & 0x1FF;
    uint64 ptIndex = (virtualAddr >> 12) & 0x1FF;

    uint64* pml4 = tableFrom(readCr3());
    uint64* pdpt = nextTable(pml4, pml4Index, 0, false);
    if (!pdpt)
        return false;
    uint64* pd = nextTable(pdpt, pdptIndex, 0, false);
    if (!pd)
        return false;
    if (pd[pdIndex] & entryPageSize)
        return (pd[pdIndex] & entryPresent) && (pd[pdIndex] & pageWrite);

    uint64* pt = nextTable(pd, pdIndex, 0, false);
    if (!pt)
        return false;
    if (!(pt[ptIndex] & entryPresent))
        return false;
    return (pt[ptIndex] & pageWrite) != 0;
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

bool pagingMapUserRangeExact(uint64 virtualAddr, uint64 size, uint64 flags)
{
    uint64 first = virtualAddr & ~(uint64)(frameSize - 1);
    uint64 last = (virtualAddr + size + frameSize - 1) & ~(uint64)(frameSize - 1);

    for (uint64 page = first; page < last; page += frameSize) {
        if (pagingResolve(page) != 0)
            continue;
        uint64 frame = pmmAlloc();
        if (frame == 0)
            return false;
        if (!pagingMap(page, frame, pageUser | flags))
            return false;
        memset((void*)page, 0, frameSize);
    }
    return true;
}

bool pagingMapUserRangeFlags(uint64 virtualAddr, uint64 size, uint64 extraFlags)
{
    return pagingMapUserRangeExact(virtualAddr, size, pageWrite | extraFlags);
}

bool pagingMapUserRange(uint64 virtualAddr, uint64 size)
{
    return pagingMapUserRangeFlags(virtualAddr, size, 0);
}

/* True if every one of a table's 512 entries is not-present -- used to
   decide whether a page-table page that unmapping just emptied out can be
   given back to the pmm itself, not just the leaf data frame it pointed
   to. Must scan the whole table, not just the entry this call touched: a
   PD table in particular can be shared by several unrelated elfSlotSize
   slots at once (a PDPT entry spans 1GB, four times a 256 MiB slot), so
   only a table that is genuinely empty end-to-end is safe to free. */
static bool tableEmpty(const uint64* table)
{
    for (uint64 i = 0; i < 512; i++)
        if (table[i] & entryPresent)
            return false;
    return true;
}

static void unmapOnePage(uint64 virtualAddr)
{
    uint64 pml4Index = (virtualAddr >> 39) & 0x1FF;
    uint64 pdptIndex = (virtualAddr >> 30) & 0x1FF;
    uint64 pdIndex = (virtualAddr >> 21) & 0x1FF;
    uint64 ptIndex = (virtualAddr >> 12) & 0x1FF;

    uint64* pml4 = tableFrom(readCr3());
    uint64* pdpt = nextTable(pml4, pml4Index, 0, false);
    if (!pdpt)
        return;
    uint64* pd = nextTable(pdpt, pdptIndex, 0, false);
    if (!pd)
        return;
    if (pd[pdIndex] & entryPageSize)
        return;

    uint64* pt = nextTable(pd, pdIndex, 0, false);
    if (!pt)
        return;
    if (!(pt[ptIndex] & entryPresent))
        return;

    pmmFree(pt[ptIndex] & addressMask);
    pt[ptIndex] = 0;
    invalidatePage(virtualAddr);

    /* Bottom-up structural cleanup: a PT/PD page-table page is itself just
       a pmm frame (see nextTable's create path), and every page
       pagingMapUserRange ever maps allocates one on demand -- leaving them
       behind once the last leaf entry they held is gone would be a second,
       smaller leak underneath the one this function exists to fix.
       Deliberately stops at PD: reclaiming a PDPT or the PML4 itself would
       mean tearing down a structure shared by up to 512GB / 1GB of
       unrelated address space respectively, which nothing in this kernel's
       single shared page table can ever actually make fully empty (the
       kernel's own mappings always live somewhere under the same PML4
       entry as every user slot). */
    if (tableEmpty(pt)) {
        pmmFree((uint64)pt);
        pd[pdIndex] = 0;

        if (tableEmpty(pd)) {
            pmmFree((uint64)pd);
            pdpt[pdptIndex] = 0;
        }
    }
}

void pagingUnmapUserRange(uint64 virtualAddr, uint64 size)
{
    uint64 first = virtualAddr & ~(uint64)(frameSize - 1);
    uint64 last = (virtualAddr + size + frameSize - 1) & ~(uint64)(frameSize - 1);

    for (uint64 page = first; page < last; page += frameSize)
        unmapOnePage(page);
}
