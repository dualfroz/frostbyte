#include "ktest.h"
#include "mmap.h"
#include "sched.h"
#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "vfs.h"
#include "fs.h"
#include "string.h"

/*
 * Coverage for mmap.c's real demand-paged mmap(2)/munmap(2) -- run entirely
 * on the idle thread (KTEST always runs there, before schedStart(), see
 * ktest.h's own file comment), whose real userSlotBase is 0 (a kernel
 * thread owns no user slot). Every test below temporarily repoints it at
 * mmapTestSlotBase, a 256 MiB (elfSlotSize)-aligned scratch address far
 * above any real binary's USER_BASE (Makefile's highest is currently under
 * 0x600000000, ~24 GiB) and far above pagingTest.c's own scratch range
 * (0x200000000-0x202000000, ~8 GiB) so the two files can never collide even
 * if their tests interleave, then restores it to 0 before returning --
 * exactly the "explicit, restorable state" every other KTEST here already
 * insists on rather than depending on hidden global setup.
 */

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

    /* Overlapping the just-reserved [base, base+2*frameSize) from either
       side must be refused -- mmapReserve never silently merges/replaces. */
    KT_FALSE(t, mmapReserve(self, base + frameSize, frameSize, mmapProtRead, true, -1, 0, &addr));
    KT_FALSE(t, mmapReserve(self, base - frameSize, 2 * frameSize, mmapProtRead, true, -1, 0, &addr));

    /* Outside the mmap region entirely (below stackTop, where the real
       stack itself lives) is refused regardless of alignment. */
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

    /* Nothing mapped yet -- mmapReserve never eagerly allocates. */
    KT_EQ(t, pagingResolve(addr), 0);

    KT_TRUE(t, mmapFaultHandler(self, addr + 42, false));
    KT_NE(t, pagingResolve(addr), 0);

    /* Demand-zero: untouched content must actually be zero, not whatever
       garbage the frame's previous owner (if any) left behind. */
    uint8* bytes = (uint8*)addr;
    for (int i = 0; i < 64; i++)
        KT_EQ(t, bytes[i], 0);

    /* A write fault on an already-present page (mmapProtWrite granted at
       reserve time) is simply a no-op success -- the page was already
       mapped pageWrite by the read fault above. */
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

    /* A read fault is fine (mmapProtRead granted)... */
    KT_TRUE(t, mmapFaultHandler(self, addr, false));
    /* ...but a write fault against the SAME already-reserved range, on a
       fresh page that never got mapped by the read above, must be refused
       -- this is exactly the case isr.c turns into a real SIGSEGV. */
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
    /* Past end-of-file within the same page: zero-padded, not garbage. */
    KT_EQ(t, ((const uint8*)addr)[strlen(payload)], 0);

    /* The fd's own seek offset must be exactly what it was before the
       fault -- mmapFaultHandler saves/restores it around its own read, so
       a caller's ordinary vfsRead/vfsLseek use of the same fd is never
       disturbed by an unrelated page fault. */
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

    /* Honestly scoped limitation (see mmap.h's own Vma doc comment): this
       mapping cannot outlive the fd that backs it. A fault against it once
       the fd is gone must fail cleanly, never silently serve zeroed or
       stale data as if it had succeeded. */
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

    /* The address range is genuinely free again, not just unmapped. */
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

    /* Unmap only the middle page -- the low and high pages must survive,
       untouched, as two now-independent Vmas. */
    KT_TRUE(t, mmapRelease(self, addr + frameSize, frameSize));

    KT_NE(t, pagingResolve(addr), 0);
    KT_EQ(t, pagingResolve(addr + frameSize), 0);
    KT_NE(t, pagingResolve(addr + 2 * frameSize), 0);

    KT_NOTNULL(t, mmapFind(self, addr));
    KT_NULL(t, mmapFind(self, addr + frameSize));
    KT_NOTNULL(t, mmapFind(self, addr + 2 * frameSize));

    /* The middle gap is genuinely free address space again. */
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
    /* mmapReleaseAll deliberately does not touch the page table -- callers
       that need the frame back reclaim it their own way first (doExec).
       The frame is still really mapped; teardownSlot's own whole-region
       pagingUnmapUserRange (not mmapReleaseAll again) is what reclaims it
       for this test. */
    KT_NE(t, pagingResolve(addr), 0);

    teardownSlot(self, previous);
}
