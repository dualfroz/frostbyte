#ifndef FROSTBYTE_ELF_H
#define FROSTBYTE_ELF_H

#include "types.h"

typedef struct Elf64Header {
    uint8 ident[16];
    uint16 type;
    uint16 machine;
    uint32 version;
    uint64 entry;
    uint64 programHeaderOffset;
    uint64 sectionHeaderOffset;
    uint32 flags;
    uint16 headerSize;
    uint16 programHeaderEntrySize;
    uint16 programHeaderCount;
    uint16 sectionHeaderEntrySize;
    uint16 sectionHeaderCount;
    uint16 sectionNameIndex;
} __attribute__((packed)) Elf64Header;

typedef struct Elf64ProgramHeader {
    uint32 type;
    uint32 flags;
    uint64 offset;
    uint64 virtualAddr;
    uint64 physicalAddr;
    uint64 fileSize;
    uint64 memorySize;
    uint64 align;
} __attribute__((packed)) Elf64ProgramHeader;

uint64 elfLoad(const void* image, uint64 imageSize);

uint64 elfImageEntry(const void* image, uint64 imageSize);

uint64 elfSegmentPagingFlags(uint32 segmentFlags);

#define elfSlotSize        0x10000000ull
#define elfSlotStackOffset 0x08000000ull
#define elfSlotBrkOffset   0x04000000ull

uint64 elfSlotBase(uint64 entry);
uint64 elfSlotStackTop(uint64 entry);
uint64 elfSlotBrkStart(uint64 entry);

#define elfMaxArgs 16

bool elfBuildInitialStack(uint64 stackTop, uint64 stackSize, const char* const* argv,
                           int argc, uint64* outRsp);

#endif
