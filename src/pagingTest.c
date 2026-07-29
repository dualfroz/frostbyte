#include "ktest.h"
#include "paging.h"
#include "pmm.h"

/*
 * Coverage for pagingUnmapUserRange (paging.c) -- the inverse of
 * pagingMapUserRange, added so threadReap/threadDiscard (sched.c) and
 * doExec (syscall.c) can actually give a dead or replaced process's
 * physical frames back to the pmm instead of leaking them forever (every
 * spawn/exec before this used to permanently consume frames, since
 * frostbyte's single shared page table is never touched on thread death).
 *
 * Addresses here are picked far above the highest real USER_BASE slot any
 * binary is linked at (0x40000000..0xF8000000, see Makefile/kmain.c) so
 * these tests can never collide with a real program's mapping, and each
 * test uses its own base so they cannot interfere with one another even
 * if run out of link order.
 */

#define pagingTestBaseA 0x200000000ull
#define pagingTestBaseB 0x201000000ull
#define pagingTestBaseC 0x202000000ull
#define pagingTestBaseD 0x203000000ull
#define pagingTestBaseE 0x204000000ull

KTEST(pagingMapUserRangeThenUnmapUserRangeReturnsFramesToPmm)
{
    uint64 before = pmmFreeFrames();

    /* At least 3 frames for the data pages themselves -- pagingTestBaseA is
       virgin address space, so the map also has to pay for whatever PDPT/
       PD/PT page-table pages do not exist yet along the way (nextTable's
       create path, paging.c), which this test does not try to predict the
       exact count of. The unmap side below is the precise assertion: every
       frame spent here, data or structural, must come all the way back. */
    KT_TRUE(t, pagingMapUserRange(pagingTestBaseA, 3 * frameSize));
    KT_LE(t, pmmFreeFrames(), before - 3);
    for (uint64 i = 0; i < 3; i++)
        KT_NE(t, pagingResolve(pagingTestBaseA + i * frameSize), 0);

    pagingUnmapUserRange(pagingTestBaseA, 3 * frameSize);
    KT_EQU(t, pmmFreeFrames(), before);
    for (uint64 i = 0; i < 3; i++)
        KT_EQ(t, pagingResolve(pagingTestBaseA + i * frameSize), 0);
}

KTEST(pagingUnmapUserRangeOnNeverMappedRangeIsSafeNoop)
{
    uint64 before = pmmFreeFrames();

    pagingUnmapUserRange(pagingTestBaseB, 5 * frameSize);

    KT_EQU(t, pmmFreeFrames(), before);
    for (uint64 i = 0; i < 5; i++)
        KT_EQ(t, pagingResolve(pagingTestBaseB + i * frameSize), 0);
}

KTEST(pagingUnmapUserRangeOnlyFreesPagesActuallyPresent)
{
    uint64 before = pmmFreeFrames();

    uint64 frame0 = pmmAlloc();
    uint64 frame2 = pmmAlloc();
    KT_NE(t, frame0, 0);
    KT_NE(t, frame2, 0);
    KT_TRUE(t, pagingMap(pagingTestBaseC, frame0, pageUser | pageWrite));
    KT_TRUE(t, pagingMap(pagingTestBaseC + 2 * frameSize, frame2, pageUser | pageWrite));
    /* pagingTestBaseC + 1*frameSize and +3*frameSize deliberately left
       unmapped -- a stand-in for the gaps a real elfSlotSize slot is full
       of (a small program's image plus its stack cover only a handful of
       the 65536 pages a whole slot spans). before already accounts for the
       two explicit pmmAlloc calls above; pagingMap may spend a couple more
       on fresh PD/PT structure for this virgin address, same caveat as the
       first test in this file -- the unmap side is the exact assertion. */

    KT_LE(t, pmmFreeFrames(), before - 2);

    pagingUnmapUserRange(pagingTestBaseC, 4 * frameSize);

    KT_EQU(t, pmmFreeFrames(), before);
    for (uint64 i = 0; i < 4; i++)
        KT_EQ(t, pagingResolve(pagingTestBaseC + i * frameSize), 0);
}

/*
 * Coverage for pageNoExecute (paging.h) and its plumbing: pagingMap's own
 * fix to stop masking bit 63 out of the leaf PTE it writes, and
 * pagingMapUserRangeFlags/pagingIsExecutable, the two entry points
 * mmap.c/kmain.c/syscall.c/jobctl.c/sched.c actually use. Only meaningful
 * when pagingNxSupported() is true (boot.S's IA32_EFER.NXE, gated on
 * CPUID.80000001H:EDX.NX) -- every QEMU/TCG target this project's own gate
 * (tools/ci-test.sh) boots under reports the feature, but a KTEST skipping
 * itself on hardware that genuinely lacks it is more honest than a KTEST
 * that would otherwise assert a bit was set when pagingMap deliberately,
 * correctly, refused to write it.
 */
KTEST(pagingMapUserRangeFlagsAppliesNoExecuteWhenNxSupported)
{
    if (!pagingNxSupported())
        return;

    KT_TRUE(t, pagingMapUserRangeFlags(pagingTestBaseD, frameSize, 0));
    KT_TRUE(t, pagingIsExecutable(pagingTestBaseD));

    pagingUnmapUserRange(pagingTestBaseD, frameSize);
    KT_TRUE(t, pagingMapUserRangeFlags(pagingTestBaseD, frameSize, pageNoExecute));
    KT_FALSE(t, pagingIsExecutable(pagingTestBaseD));

    pagingUnmapUserRange(pagingTestBaseD, frameSize);
}

KTEST(pagingIsExecutableIsFalseForUnmappedAddress)
{
    KT_FALSE(t, pagingIsExecutable(pagingTestBaseD + 7 * frameSize));
}

/*
 * Coverage for pagingMapUserRangeExact and pagingIsWritable: unlike
 * pagingMapUserRangeFlags (which always forces pageWrite on), Exact is the
 * one entry point that can produce a genuinely READ-ONLY user mapping --
 * the primitive elf.c's elfLoad needs for a binary's own .text/.rodata
 * PT_LOAD segment. Proven directly here via pagingIsWritable, the same
 * "deterministic introspection instead of only a live fault" approach
 * pagingIsExecutable already established above; user/elfwritevictim.c is
 * the live, real-SIGSEGV proof that a genuinely read-only mapping refuses
 * a ring-3 write, not just that this bit got written.
 */
KTEST(pagingMapUserRangeExactOmitsWriteWhenNotRequested)
{
    KT_TRUE(t, pagingMapUserRangeExact(pagingTestBaseE, frameSize, 0));
    KT_FALSE(t, pagingIsWritable(pagingTestBaseE));
    KT_TRUE(t, pagingIsExecutable(pagingTestBaseE));

    pagingUnmapUserRange(pagingTestBaseE, frameSize);
}

KTEST(pagingMapUserRangeExactAppliesWriteAndNoExecuteTogether)
{
    KT_TRUE(t, pagingMapUserRangeExact(pagingTestBaseE, frameSize, pageWrite | pageNoExecute));
    KT_TRUE(t, pagingIsWritable(pagingTestBaseE));
    if (pagingNxSupported())
        KT_FALSE(t, pagingIsExecutable(pagingTestBaseE));

    pagingUnmapUserRange(pagingTestBaseE, frameSize);
}

KTEST(pagingIsWritableIsFalseForUnmappedAddress)
{
    KT_FALSE(t, pagingIsWritable(pagingTestBaseE + 7 * frameSize));
}
