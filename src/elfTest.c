#include "ktest.h"
#include "elf.h"
#include "heap.h"
#include "string.h"
#include "paging.h"

KTEST(elfSlotBaseFloorsToSlotBoundary)
{
    KT_EQU(t, elfSlotBase(0x40000000ull), 0x40000000ull);
    KT_EQU(t, elfSlotBase(0x40001000ull), 0x40000000ull);
    KT_EQU(t, elfSlotBase(0x4FFFFFFFull), 0x40000000ull);
    KT_EQU(t, elfSlotBase(0x50000000ull), 0x50000000ull);
    KT_EQU(t, elfSlotBase(0xB0000ABCull), 0xB0000000ull);
}

KTEST(elfSlotStackAndBrkMatchBootTimeConvention)
{
    KT_EQU(t, elfSlotStackTop(0x60001000ull), 0x68000000ull);
    KT_EQU(t, elfSlotBrkStart(0x60001000ull), 0x64000000ull);

    KT_EQU(t, elfSlotStackTop(0x40000abcull), 0x48000000ull);
    KT_EQU(t, elfSlotBrkStart(0x40000abcull), 0x44000000ull);

    KT_EQU(t, elfSlotStackTop(0x0FFFFFFFull), 0x08000000ull);
    KT_EQU(t, elfSlotBrkStart(0x0FFFFFFFull), 0x04000000ull);
}

KTEST(elfBuildInitialStackLaysOutArgcArgvAndStringsCorrectly)
{
    uint64 stackSize = 4096;
    uint8* stack = (uint8*)kmalloc(stackSize);
    uint64 stackTop = (uint64)stack + stackSize;

    const char* argv[3] = { "/bin/argtest", "alpha", "beta" };
    uint64 rsp = 0;
    KT_TRUE(t, elfBuildInitialStack(stackTop, stackSize, argv, 3, &rsp));

    KT_GE(t, rsp, (uint64)stack);
    KT_LT(t, rsp, stackTop);
    KT_EQU(t, rsp % 16, 0);

    const uint64* slots = (const uint64*)rsp;
    KT_EQ(t, (int64)slots[0], 3);
    KT_STREQ(t, (const char*)slots[1], "/bin/argtest");
    KT_STREQ(t, (const char*)slots[2], "alpha");
    KT_STREQ(t, (const char*)slots[3], "beta");
    KT_EQU(t, slots[4], 0);

    kfree(stack);
}

KTEST(elfBuildInitialStackWithZeroArgcStillTerminatesArgv)
{
    uint64 stackSize = 256;
    uint8* stack = (uint8*)kmalloc(stackSize);
    uint64 stackTop = (uint64)stack + stackSize;

    uint64 rsp = 0;
    KT_TRUE(t, elfBuildInitialStack(stackTop, stackSize, nullptr, 0, &rsp));
    KT_EQU(t, rsp % 16, 0);

    const uint64* slots = (const uint64*)rsp;
    KT_EQ(t, (int64)slots[0], 0);
    KT_EQU(t, slots[1], 0);

    kfree(stack);
}

KTEST(elfBuildInitialStackRejectsArgcAboveElfMaxArgs)
{
    uint64 stackSize = 4096;
    uint8* stack = (uint8*)kmalloc(stackSize);
    uint64 stackTop = (uint64)stack + stackSize;

    const char* argv[1] = { "x" };
    uint64 rsp = 0;
    KT_FALSE(t, elfBuildInitialStack(stackTop, stackSize, argv, elfMaxArgs + 1, &rsp));
    KT_FALSE(t, elfBuildInitialStack(stackTop, stackSize, argv, -1, &rsp));

    kfree(stack);
}

KTEST(elfBuildInitialStackRejectsArgvTooBigForStack)
{
    uint64 stackSize = 8;
    uint8* stack = (uint8*)kmalloc(stackSize);
    uint64 stackTop = (uint64)stack + stackSize;

    const char* argv[1] = { "/bin/argtest" };
    uint64 rsp = 0xDEADBEEFull;
    KT_FALSE(t, elfBuildInitialStack(stackTop, stackSize, argv, 1, &rsp));
    KT_EQU(t, rsp, 0xDEADBEEFull);

    kfree(stack);
}

KTEST(elfSegmentPagingFlagsReadExecuteIsWritableNeverAndAlwaysExecutable)
{
    KT_EQU(t, elfSegmentPagingFlags(5), 0ull);
}

KTEST(elfSegmentPagingFlagsReadWriteIsWritableAndNeverExecutable)
{
    KT_EQU(t, elfSegmentPagingFlags(6), pageWrite | pageNoExecute);
}

KTEST(elfSegmentPagingFlagsReadOnlyIsNeitherWritableNorExecutable)
{
    KT_EQU(t, elfSegmentPagingFlags(4), pageNoExecute);
}

KTEST(elfSegmentPagingFlagsReadWriteExecuteIsWritableAndExecutable)
{
    KT_EQU(t, elfSegmentPagingFlags(7), pageWrite);
}

KTEST(elfImageEntryReadsTheEntryAndRejectsBadImages)
{
    Elf64Header header;
    memset(&header, 0, sizeof(header));
    header.ident[0] = 0x7F;
    header.ident[1] = 'E';
    header.ident[2] = 'L';
    header.ident[3] = 'F';
    header.ident[4] = 2;
    header.type = 2;
    header.machine = 62;
    header.entry = 0x700002000ull;

    KT_EQU(t, elfImageEntry(&header, sizeof(header)), 0x700002000ull);
    KT_EQU(t, elfImageEntry(&header, sizeof(header) - 1), 0ull);

    header.ident[1] = 0;
    KT_EQU(t, elfImageEntry(&header, sizeof(header)), 0ull);
}
