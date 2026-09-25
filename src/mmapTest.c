#include "ktest.h"
#include "mmap.h"
#include "sched.h"
#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "vfs.h"
#include "fs.h"
#include "string.h"

#define mmapTestSlotBase 0x300000000000ull

static uint64 setupSlot(Thread* t)
{
    uint64 previous = t->userSlotBase;
    t->userSlotBase = mmapTestSlotBase;
    mmapReleaseAll(t);
    return previous;
}

static void teardownSlot(Thread* t, uint64 previous)
{
    pagingUnmapUserRange(mmapTestSlotBase, elfSlotSize);
    mmapReleaseAll(t);
    t->userSlotBase = previous;
}

KTEST(mmapReserveRejectsMisalignedZeroAndOverlap)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    uint64 addr;
    KT_FALSE(t, mmapReserve(self, 1, frameSize, mmapProtRead, true, -1, 0, &addr));
    KT_FALSE(t, mmapReserve(self, 0, 0, mmapProtRead, true, -1, 0, &addr));
    KT_FALSE(t, mmapReserve(self, mmapTestSlotBase + elfSlotStackOffset, frameSize + 1, mmapProtRead, true, -1, 0, &addr));

    uint64 base = mmapTestSlotBase + elfSlotStackOffset;
    KT_TRUE(t, mmapReserve(self, base, 2 * frameSize, mmapProtRead | mmapProtWrite, true, -1, 0, &addr));
    KT_EQU(t, addr, base);

    KT_FALSE(t, mmapReserve(self, base + frameSize, frameSize, mmapProtRead, true, -1, 0, &addr));
    KT_FALSE(t, mmapReserve(self, base - frameSize, 2 * frameSize, mmapProtRead, true, -1, 0, &addr));

    KT_FALSE(t, mmapReserve(self, mmapTestSlotBase, frameSize, mmapProtRead, true, -1, 0, &addr));

    teardownSlot(self, previous);
}

KTEST(mmapReserveAutoPlacementIsFirstFitAndNonOverlapping)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    uint64 firstAddr, secondAddr;
    KT_TRUE(t, mmapReserve(self, 0, 3 * frameSize, mmapProtRead, true, -1, 0, &firstAddr));
    KT_TRUE(t, mmapReserve(self, 0, frameSize, mmapProtRead, true, -1, 0, &secondAddr));

    KT_EQU(t, firstAddr, mmapTestSlotBase + elfSlotStackOffset);
    KT_EQU(t, secondAddr, firstAddr + 3 * frameSize);

    teardownSlot(self, previous);
}

KTEST(mmapFaultHandlerAnonymousDemandZeroFillThenWritable)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    uint64 addr;
    KT_TRUE(t, mmapReserve(self, 0, frameSize, mmapProtRead | mmapProtWrite, true, -1, 0, &addr));

    KT_EQ(t, pagingResolve(addr), 0);

    KT_TRUE(t, mmapFaultHandler(self, addr + 42, false));
    KT_NE(t, pagingResolve(addr), 0);

    uint8* bytes = (uint8*)addr;
    for (int i = 0; i < 64; i++)
        KT_EQ(t, bytes[i], 0);

    KT_TRUE(t, mmapFaultHandler(self, addr, true));
    bytes[0] = 0xAB;
    KT_EQ(t, bytes[0], 0xAB);

    teardownSlot(self, previous);
}

KTEST(mmapFaultHandlerSetsNoExecuteUnlessProtExecGranted)
{
    if (!pagingNxSupported())
        return;

    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    uint64 noExecAddr;
    KT_TRUE(t, mmapReserve(self, 0, frameSize, mmapProtRead | mmapProtWrite, true, -1, 0, &noExecAddr));
    KT_TRUE(t, mmapFaultHandler(self, noExecAddr, false));
    KT_FALSE(t, pagingIsExecutable(noExecAddr));

    uint64 execAddr;
    KT_TRUE(t, mmapReserve(self, 0, frameSize, mmapProtRead | mmapProtWrite | mmapProtExec, true, -1, 0, &execAddr));
    KT_TRUE(t, mmapFaultHandler(self, execAddr, false));
    KT_TRUE(t, pagingIsExecutable(execAddr));

    teardownSlot(self, previous);
}

KTEST(mmapFaultHandlerRefusesWriteAgainstReadOnlyMapping)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    uint64 addr;
    KT_TRUE(t, mmapReserve(self, 0, frameSize, mmapProtRead, true, -1, 0, &addr));

    KT_TRUE(t, mmapFaultHandler(self, addr, false));
    uint64 secondAddr;
    KT_TRUE(t, mmapReserve(self, 0, frameSize, mmapProtRead, true, -1, 0, &secondAddr));
    KT_FALSE(t, mmapFaultHandler(self, secondAddr, true));
    KT_EQ(t, pagingResolve(secondAddr), 0);

    teardownSlot(self, previous);
}

KTEST(mmapFaultHandlerRefusesAddressOutsideAnyVma)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    KT_FALSE(t, mmapFaultHandler(self, mmapTestSlotBase + elfSlotStackOffset + 5 * frameSize, false));

    teardownSlot(self, previous);
}

KTEST(mmapFaultHandlerFileBackedReadsRealFileContent)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    const char* payload = "mmapTest file-backed page content, byte-for-byte.";
    int fd = vfsOpen("/tmp/mmaptest-src.txt", oReadWrite | oCreate | oTrunc);
    KT_TRUE(t, fd >= 0);
    KT_EQ(t, vfsWrite(fd, payload, strlen(payload)), (int64)strlen(payload));

    uint64 addr;
    KT_TRUE(t, mmapReserve(self, 0, frameSize, mmapProtRead, false, fd, 0, &addr));
    KT_TRUE(t, mmapFaultHandler(self, addr, false));

    KT_TRUE(t, memcmp((const void*)addr, payload, strlen(payload)) == 0);
    KT_EQ(t, ((const uint8*)addr)[strlen(payload)], 0);

    KT_EQ(t, vfsLseek(fd, 0, seekCur), (int64)strlen(payload));

    vfsClose(fd);
    teardownSlot(self, previous);
}

KTEST(mmapFaultHandlerRefusesFileBackedReadAfterFdClosed)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    int fd = vfsOpen("/tmp/mmaptest-closed.txt", oReadWrite | oCreate | oTrunc);
    KT_TRUE(t, fd >= 0);
    KT_EQ(t, vfsWrite(fd, "x", 1), 1);

    uint64 addr;
    KT_TRUE(t, mmapReserve(self, 0, frameSize, mmapProtRead, false, fd, 0, &addr));
    vfsClose(fd);

    KT_FALSE(t, mmapFaultHandler(self, addr, false));
    KT_EQ(t, pagingResolve(addr), 0);

    teardownSlot(self, previous);
}

KTEST(mmapReleaseFreesFramesAndAllowsReReservation)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);
    uint64 framesBefore = pmmFreeFrames();

    uint64 addr;
    KT_TRUE(t, mmapReserve(self, 0, 2 * frameSize, mmapProtRead | mmapProtWrite, true, -1, 0, &addr));
    KT_TRUE(t, mmapFaultHandler(self, addr, false));
    KT_TRUE(t, mmapFaultHandler(self, addr + frameSize, false));
    KT_LE(t, pmmFreeFrames(), framesBefore - 2);

    KT_TRUE(t, mmapRelease(self, addr, 2 * frameSize));
    KT_EQU(t, pmmFreeFrames(), framesBefore);
    KT_EQ(t, pagingResolve(addr), 0);
    KT_EQ(t, pagingResolve(addr + frameSize), 0);
    KT_NULL(t, mmapFind(self, addr));

    uint64 secondAddr;
    KT_TRUE(t, mmapReserve(self, addr, 2 * frameSize, mmapProtRead, true, -1, 0, &secondAddr));
    KT_EQU(t, secondAddr, addr);

    teardownSlot(self, previous);
}

KTEST(mmapReleaseOnNeverMappedRangeIsSafeNoop)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);
    uint64 framesBefore = pmmFreeFrames();

    uint64 probe = mmapTestSlotBase + elfSlotStackOffset + 10 * frameSize;
    KT_TRUE(t, mmapRelease(self, probe, 3 * frameSize));
    KT_EQU(t, pmmFreeFrames(), framesBefore);

    teardownSlot(self, previous);
}

KTEST(mmapReleaseSplitsMiddleOfAVmaIntoTwo)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    uint64 addr;
    KT_TRUE(t, mmapReserve(self, 0, 3 * frameSize, mmapProtRead | mmapProtWrite, true, -1, 0, &addr));
    KT_TRUE(t, mmapFaultHandler(self, addr, false));
    KT_TRUE(t, mmapFaultHandler(self, addr + frameSize, false));
    KT_TRUE(t, mmapFaultHandler(self, addr + 2 * frameSize, false));

    KT_TRUE(t, mmapRelease(self, addr + frameSize, frameSize));

    KT_NE(t, pagingResolve(addr), 0);
    KT_EQ(t, pagingResolve(addr + frameSize), 0);
    KT_NE(t, pagingResolve(addr + 2 * frameSize), 0);

    KT_NOTNULL(t, mmapFind(self, addr));
    KT_NULL(t, mmapFind(self, addr + frameSize));
    KT_NOTNULL(t, mmapFind(self, addr + 2 * frameSize));

    uint64 refilled;
    KT_TRUE(t, mmapReserve(self, addr + frameSize, frameSize, mmapProtRead, true, -1, 0, &refilled));

    teardownSlot(self, previous);
}

KTEST(mmapReserveFailsOnceVmaTableIsFull)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    uint64 addr;
    for (int i = 0; i < maxVmasPerThread; i++)
        KT_TRUE(t, mmapReserve(self, 0, frameSize, mmapProtRead, true, -1, 0, &addr));

    KT_FALSE(t, mmapReserve(self, 0, frameSize, mmapProtRead, true, -1, 0, &addr));

    teardownSlot(self, previous);
}

KTEST(mmapReleaseAllClearsBookkeepingOnly)
{
    Thread* self = currentThread();
    uint64 previous = setupSlot(self);

    uint64 addr;
    KT_TRUE(t, mmapReserve(self, 0, frameSize, mmapProtRead, true, -1, 0, &addr));
    KT_TRUE(t, mmapFaultHandler(self, addr, false));
    KT_NE(t, pagingResolve(addr), 0);

    mmapReleaseAll(self);

    KT_NULL(t, mmapFind(self, addr));
    KT_NE(t, pagingResolve(addr), 0);

    teardownSlot(self, previous);
}
