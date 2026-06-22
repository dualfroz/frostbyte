#include "elf.h"
#include "paging.h"
#include "string.h"
#include "console.h"

#define programTypeLoad 1
#define elfTypeExec 2
#define elfMachineX8664 62

/* Standard ELF64 Elf64ProgramHeader.flags bits (p_flags) -- PF_X/PF_W/PF_R
   by their real numeric values, not this kernel's own invention. PF_R is
   never consulted below: every mapping pagingMapUserRangeExact creates is
   already readable by virtue of pageUser alone (x86_64 has no separate
   read-disable outside SMAP/PKU, neither of which this kernel uses), so a
   segment missing PF_R would be indistinguishable from one that has it. */
#define elfFlagExecute 1
#define elfFlagWrite   2

static bool validHeader(const Elf64Header* header)
{
    if (header->ident[0] != 0x7F || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F')
        return false;
    if (header->ident[4] != 2)
        return false;
    if (header->type != elfTypeExec)
        return false;
    if (header->machine != elfMachineX8664)
        return false;
    return true;
}

/* Translates one ELF64 program header's own p_flags into the extra
   paging.h leaf-PTE bits pagingMapUserRangeExact needs to map that exact
   segment with real per-segment W^X: writable iff PF_W was set, and
   pageNoExecute UNLESS PF_X was set (never both writable and executable
   at once for a segment that asked for neither, or for one that asked
   for only PF_W -- data/bss segments -- which is the whole point).
   Pulled out as its own pure function, no paging/scheduler/VFS involved,
   so elfTest.c can exercise every PF_X/PF_W combination deterministically
   before the scheduler even starts, the same way this file's other pure
   address-arithmetic helpers (elfSlotBase and friends) already are. */
uint64 elfSegmentPagingFlags(uint32 segmentFlags)
{
    uint64 flags = 0;
    if (segmentFlags & elfFlagWrite)
        flags |= pageWrite;
    if (!(segmentFlags & elfFlagExecute))
        flags |= pageNoExecute;
    return flags;
}

uint64 elfLoad(const void* image, uint64 imageSize)
{
    const Elf64Header* header = (const Elf64Header*)image;

    if (imageSize < sizeof(Elf64Header) || !validHeader(header)) {
        kprintf("[elf]  rejected image: not a static x86_64 ELF\n");
        return 0;
    }

    const uint8* base = (const uint8*)image;

    for (uint16 i = 0; i < header->programHeaderCount; i++) {
        const Elf64ProgramHeader* segment = (const Elf64ProgramHeader*)
            (base + header->programHeaderOffset + (uint64)i * header->programHeaderEntrySize);

        if (segment->type != programTypeLoad)
            continue;

        uint64 mapFlags = elfSegmentPagingFlags(segment->flags);
        if (!pagingMapUserRangeExact(segment->virtualAddr, segment->memorySize, mapFlags)) {
            kprintf("[elf]  failed to map segment at %p\n", (void*)segment->virtualAddr);
            return 0;
        }

        memcpy((void*)segment->virtualAddr, base + segment->offset, segment->fileSize);
    }

    return header->entry;
}

uint64 elfSlotBase(uint64 entry)
{
    return entry & ~(elfSlotSize - 1);
}

uint64 elfSlotStackTop(uint64 entry)
{
    return elfSlotBase(entry) + elfSlotStackOffset;
}

uint64 elfSlotBrkStart(uint64 entry)
{
    return elfSlotBase(entry) + elfSlotBrkOffset;
}

bool elfBuildInitialStack(uint64 stackTop, uint64 stackSize, const char* const* argv,
                           int argc, uint64* outRsp)
{
    if (argc < 0 || argc > elfMaxArgs)
        return false;

    uint64 stackBottom = stackTop - stackSize;
    uint64 argvAddr[elfMaxArgs];
    uint64 cursor = stackTop;

    /* Strings first, growing downward from the top of the stack -- each
       one's address is remembered so the pointer array below can point at
       it once every string is placed. */
    for (int i = argc - 1; i >= 0; i--) {
        uint64 len = strlen(argv[i]) + 1;
        if (cursor - stackBottom < len)
            return false;
        cursor -= len;
        memcpy((void*)cursor, argv[i], len);
        argvAddr[i] = cursor;
    }

    /* argc slot + argv[0..argc-1] + a NULL terminator, SysV-style. Rounded
       down to 16 bytes because that is where _start (crt.S) expects rsp to
       sit at process entry (so that its own later `call main` leaves main
       looking at the usual post-call rsp%16==8). */
    uint64 fixedAreaBytes = (uint64)(argc + 2) * sizeof(uint64);
    if (cursor - stackBottom < fixedAreaBytes)
        return false;
    uint64 base = (cursor - fixedAreaBytes) & ~15ull;
    if (base < stackBottom)
        return false;

    uint64* slots = (uint64*)base;
    slots[0] = (uint64)argc;
    for (int i = 0; i < argc; i++)
        slots[1 + i] = argvAddr[i];
    slots[1 + argc] = 0;

    *outRsp = base;
    return true;
}
