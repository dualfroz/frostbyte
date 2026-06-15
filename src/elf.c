#include "elf.h"
#include "paging.h"
#include "string.h"
#include "console.h"

#define programTypeLoad 1
#define elfTypeExec 2
#define elfMachineX8664 62

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

        if (!pagingMapUserRange(segment->virtualAddr, segment->memorySize)) {
            kprintf("[elf]  failed to map segment at %p\n", (void*)segment->virtualAddr);
            return 0;
        }

        memcpy((void*)segment->virtualAddr, base + segment->offset, segment->fileSize);
    }

    return header->entry;
}
