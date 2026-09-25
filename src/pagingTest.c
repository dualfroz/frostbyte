#include "ktest.h"
#include "paging.h"
#include "pmm.h"

#define pagingTestBaseA 0x200000000ull
#define pagingTestBaseB 0x201000000ull
#define pagingTestBaseC 0x202000000ull
#define pagingTestBaseD 0x203000000ull
#define pagingTestBaseE 0x204000000ull

KTEST(pagingMapUserRangeThenUnmapUserRangeReturnsFramesToPmm)
{
    uint64 before = pmmFreeFrames();

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

    KT_LE(t, pmmFreeFrames(), before - 2);

    pagingUnmapUserRange(pagingTestBaseC, 4 * frameSize);

    KT_EQU(t, pmmFreeFrames(), before);
    for (uint64 i = 0; i < 4; i++)
        KT_EQ(t, pagingResolve(pagingTestBaseC + i * frameSize), 0);
}

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
