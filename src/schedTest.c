#include "ktest.h"
#include "sched.h"
#include "ports.h"
#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "pipe.h"
#include "signal.h"
#include "pit.h"
#include "klib.h"

KTEST(schedBlockCurrentMarksBlockedWithChannel)
{
    ThreadState saved = currentThread()->state;
    int channel;

    schedBlockCurrent(&channel);
    KT_EQ(t, currentThread()->state, threadBlocked);
    KT_EQP(t, currentThread()->waitChannel, &channel);

    schedWakeChannel(&channel);
    currentThread()->state = saved;
}

KTEST(schedWakeChannelIgnoresMismatchedChannel)
{
    ThreadState saved = currentThread()->state;
    int channelA;
    int channelB;

    schedBlockCurrent(&channelA);
    KT_EQ(t, currentThread()->state, threadBlocked);

    schedWakeChannel(&channelB);
    KT_EQ(t, currentThread()->state, threadBlocked);
    KT_EQP(t, currentThread()->waitChannel, &channelA);

    schedWakeChannel(&channelA);
    KT_EQ(t, currentThread()->state, threadRunnable);

    currentThread()->state = saved;
}

KTEST(schedWakeChannelClearsWaitChannelOnWake)
{
    ThreadState saved = currentThread()->state;
    int channel;

    schedBlockCurrent(&channel);
    schedWakeChannel(&channel);
    KT_EQ(t, currentThread()->state, threadRunnable);
    KT_NULL(t, currentThread()->waitChannel);

    currentThread()->state = saved;
}

KTEST(schedPollChannelIsStableAndUsableLikeAnyOtherChannel)
{
    ThreadState saved = currentThread()->state;

    KT_EQP(t, schedPollChannel(), schedPollChannel());
    KT_NOTNULL(t, schedPollChannel());

    schedBlockCurrent(schedPollChannel());
    KT_EQ(t, currentThread()->state, threadBlocked);
    KT_EQP(t, currentThread()->waitChannel, schedPollChannel());

    schedWakeChannel(schedPollChannel());
    KT_EQ(t, currentThread()->state, threadRunnable);

    currentThread()->state = saved;
}

KTEST(schedWakeChannelWithNoWaiterIsHarmlessNoop)
{
    ThreadState saved = currentThread()->state;
    int channel;

    schedWakeChannel(&channel);
    KT_EQ(t, currentThread()->state, saved);

    currentThread()->state = saved;
}

KTEST(schedBlockCurrentIsIdempotentUnderRepeatedCalls)
{
    ThreadState saved = currentThread()->state;
    int channelA;
    int channelB;

    schedBlockCurrent(&channelA);
    schedBlockCurrent(&channelB);
    KT_EQ(t, currentThread()->state, threadBlocked);
    KT_EQP(t, currentThread()->waitChannel, &channelB);

    schedWakeChannel(&channelA);
    KT_EQ(t, currentThread()->state, threadBlocked);

    schedWakeChannel(&channelB);
    KT_EQ(t, currentThread()->state, threadRunnable);

    currentThread()->state = saved;
}

static void noopThreadEntry(void)
{
    for (;;)
        halt();
}

KTEST(threadCreateExhaustsThenThreadDiscardFreesSlotsBackForReuse)
{
    uint32 capacity = threadCapacity();
    uint32 created[capacity];
    uint32 count = 0;

    while (count < capacity) {
        uint32 id = threadCreate("ktestDummy", noopThreadEntry);
        if (id == threadInvalidId)
            break;
        created[count++] = id;
    }

    KT_EQU(t, threadCreate("shouldFail", noopThreadEntry), threadInvalidId);

    for (uint32 i = 0; i < count; i++)
        KT_TRUE(t, threadDiscard(created[i]));

    uint32 reused = threadCreate("afterDiscard", noopThreadEntry);
    KT_NE(t, reused, threadInvalidId);
    KT_TRUE(t, threadDiscard(reused));
}

KTEST(threadCreateSkipsAFreshlyFreedLowSlotInFavorOfMovingForward)
{
    uint32 a = threadCreate("rrA", noopThreadEntry);
    uint32 b = threadCreate("rrB", noopThreadEntry);
    uint32 c = threadCreate("rrC", noopThreadEntry);
    KT_LT(t, a, b);
    KT_LT(t, b, c);

    KT_TRUE(t, threadDiscard(b));

    uint32 afterFree = threadCreate("rrAfterFree", noopThreadEntry);
    KT_NE(t, afterFree, b);
    KT_GT(t, afterFree, c);

    KT_TRUE(t, threadDiscard(a));
    KT_TRUE(t, threadDiscard(afterFree));
    KT_TRUE(t, threadDiscard(c));

    KT_EQU(t, threadPgid(b), threadInvalidId);
}

KTEST(threadCreateReusesTheOnlyFreedSlotOnceTheTableIsFull)
{
    uint32 capacity = threadCapacity();
    uint32 created[capacity];
    uint32 count = 0;

    while (count < capacity) {
        uint32 id = threadCreate("rrFill", noopThreadEntry);
        if (id == threadInvalidId)
            break;
        created[count++] = id;
    }
    KT_EQU(t, threadCreate("rrShouldFail", noopThreadEntry), threadInvalidId);

    uint32 freedId = created[count / 2];
    KT_TRUE(t, threadDiscard(freedId));

    uint32 reused = threadCreate("rrReusesOnlyFree", noopThreadEntry);
    KT_EQ(t, reused, freedId);

    for (uint32 i = 0; i < count; i++) {
        if (created[i] != freedId)
            KT_TRUE(t, threadDiscard(created[i]));
    }
    KT_TRUE(t, threadDiscard(reused));
}

KTEST(threadDiscardRejectsIdleOutOfRangeAndDoubleDiscard)
{
    KT_FALSE(t, threadDiscard(0));

    KT_FALSE(t, threadDiscard(threadCapacity()));
    KT_FALSE(t, threadDiscard(threadCapacity() + 1000));

    uint32 id = threadCreate("discardMe", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_TRUE(t, threadDiscard(id));

    KT_FALSE(t, threadDiscard(id));
}

KTEST(threadReapRejectsIdleOutOfRangeAndNeverCreatedSlots)
{
    KT_FALSE(t, threadReap(0));

    KT_FALSE(t, threadReap(threadCapacity()));
    KT_FALSE(t, threadReap(threadCapacity() + 1000));

    uint32 neverUsed = threadCount();
    if (neverUsed < threadCapacity())
        KT_FALSE(t, threadReap(neverUsed));

    uint32 runnable = threadCreate("notDeadYet", noopThreadEntry);
    KT_NE(t, runnable, threadInvalidId);
    KT_FALSE(t, threadReap(runnable));
    KT_TRUE(t, threadDiscard(runnable));
}

KTEST(threadJoinRejectsInvalidIdsWithoutBlocking)
{
    KT_EQ(t, threadJoin(0), -1);
    KT_EQ(t, threadJoin(threadCapacity()), -1);

    uint32 neverUsed = threadCount();
    if (neverUsed < threadCapacity())
        KT_EQ(t, threadJoin(neverUsed), -1);
}

KTEST(threadJoinOnDiscardedSlotFailsCleanly)
{
    uint32 id = threadCreate("transient", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_TRUE(t, threadDiscard(id));

    KT_EQ(t, threadJoin(id), -1);
}

KTEST(threadJoinReturnsRunningWhenAWakeLeftTheCallerMarkedBlocked)
{
    uint32 id = threadCreate("joinTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    threadAt(id)->exitCode = 5;
    threadAt(id)->state = threadDead;

    schedBlockCurrent(threadAt(id));
    KT_EQ(t, currentThread()->state, threadBlocked);

    KT_EQ(t, threadJoin(id), 5);
    KT_EQ(t, currentThread()->state, threadRunning);
    KT_NULL(t, currentThread()->waitChannel);
    KT_EQ(t, threadAt(id)->state, threadUnused);
}

KTEST(threadJoinOnAGoneTargetDoesNotLeaveTheCallerBlocked)
{
    uint32 id = threadCreate("goneTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_TRUE(t, threadDiscard(id));

    schedBlockCurrent(threadAt(id));
    KT_EQ(t, threadJoin(id), -1);
    KT_EQ(t, currentThread()->state, threadRunning);
    KT_NULL(t, currentThread()->waitChannel);
}

KTEST(threadCreateUserSetsUserSlotBaseFromEntry)
{
    uint64 entry = 0x320000000ull + 0x2000;
    uint32 id = threadCreateUser("probe", entry, elfSlotStackTop(entry));
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadUserSlotBase(id), elfSlotBase(entry));
    KT_EQU(t, threadUserSlotBase(id), 0x320000000ull);
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadUserSlotInUseHoldsFromCreateUntilTheThreadIsGone)
{
    uint64 slotBase = 0x700000000ull;
    uint64 entry = slotBase + 0x2000;
    KT_FALSE(t, threadUserSlotInUse(slotBase));

    uint32 id = threadCreateUser("slotProbe", entry, elfSlotStackTop(entry));
    KT_NE(t, id, threadInvalidId);
    KT_TRUE(t, threadUserSlotInUse(slotBase));
    KT_FALSE(t, threadUserSlotInUse(slotBase + elfSlotSize));

    threadAt(id)->state = threadDead;
    KT_TRUE(t, threadUserSlotInUse(slotBase));

    threadAt(id)->state = threadRunnable;
    KT_TRUE(t, threadDiscard(id));
    KT_FALSE(t, threadUserSlotInUse(slotBase));
}

KTEST(threadReapOnDeadUserThreadReclaimsItsMappedPages)
{
    uint64 fakeSlotBase = 0x330000000ull;
    uint64 entry = fakeSlotBase + 0x2000;
    uint64 stackTop = elfSlotStackTop(entry);
    uint64 stackSize = 4 * frameSize;
    uint64 before = pmmFreeFrames();

    KT_TRUE(t, pagingMapUserRange(entry, frameSize));
    KT_TRUE(t, pagingMapUserRange(stackTop - stackSize, stackSize));
    KT_LE(t, pmmFreeFrames(), before - 1 - (stackSize / frameSize));

    uint32 id = threadCreateUser("fakeProc", entry, stackTop);
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadUserSlotBase(id), fakeSlotBase);

    threadAt(id)->state = threadDead;

    KT_TRUE(t, threadReap(id));
    KT_EQU(t, pmmFreeFrames(), before);
    KT_EQ(t, pagingResolve(entry), 0);
    KT_EQ(t, pagingResolve(stackTop - 1), 0);
}

KTEST(threadDiscardOnNeverRunUserThreadReclaimsItsMappedPages)
{
    uint64 fakeSlotBase = 0x340000000ull;
    uint64 entry = fakeSlotBase + 0x2000;
    uint64 stackTop = elfSlotStackTop(entry);
    uint64 stackSize = 4 * frameSize;
    uint64 before = pmmFreeFrames();

    KT_TRUE(t, pagingMapUserRange(entry, frameSize));
    KT_TRUE(t, pagingMapUserRange(stackTop - stackSize, stackSize));
    KT_LE(t, pmmFreeFrames(), before - 1 - (stackSize / frameSize));

    uint32 id = threadCreateUser("fakeProc2", entry, stackTop);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, threadDiscard(id));
    KT_EQU(t, pmmFreeFrames(), before);
    KT_EQ(t, pagingResolve(entry), 0);
    KT_EQ(t, pagingResolve(stackTop - 1), 0);
}

KTEST(threadCreateKernelThreadHasNoUserSlotAndReapLeavesPmmUntouched)
{
    uint64 before = pmmFreeFrames();

    uint32 id = threadCreate("kernelOnly", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadUserSlotBase(id), 0);

    threadAt(id)->state = threadDead;
    KT_TRUE(t, threadReap(id));
    KT_EQU(t, pmmFreeFrames(), before);
}

KTEST(threadKillRejectsIdleOutOfRangeAndNeverCreatedSlots)
{
    KT_FALSE(t, threadKill(0));
    KT_FALSE(t, threadKill(threadCapacity()));
    KT_FALSE(t, threadKill(threadCapacity() + 1000));

    uint32 neverUsed = threadCount();
    if (neverUsed < threadCapacity())
        KT_FALSE(t, threadKill(neverUsed));
}

KTEST(threadKillMarksRunnableThreadDeadWithKilledExitCode)
{
    uint32 id = threadCreate("killMe", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, threadKill(id));
    KT_EQ(t, threadAt(id)->state, threadDead);
    KT_EQU(t, threadAt(id)->exitCode, threadKilledExitCode);

    KT_TRUE(t, threadReap(id));
}

KTEST(threadKillOnAlreadyDeadOrUnusedSlotFails)
{
    uint32 id = threadCreate("killTwice", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, threadKill(id));
    KT_FALSE(t, threadKill(id));
    KT_EQU(t, threadAt(id)->exitCode, threadKilledExitCode);

    KT_TRUE(t, threadReap(id));
    KT_FALSE(t, threadKill(id));
}

KTEST(threadKillWakesAThreadParkedInJoin)
{
    ThreadState saved = currentThread()->state;

    uint32 victim = threadCreate("victim", noopThreadEntry);
    KT_NE(t, victim, threadInvalidId);

    schedBlockCurrent(threadAt(victim));
    KT_EQ(t, currentThread()->state, threadBlocked);

    KT_TRUE(t, threadKill(victim));
    KT_EQ(t, currentThread()->state, threadRunnable);

    currentThread()->state = saved;
    KT_TRUE(t, threadReap(victim));
}

KTEST(threadCreateAttributesParentIdToWhicheverThreadIsActuallyCurrent)
{
    uint32 id = threadCreate("childOfCurrent", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadAt(id)->parentId, currentThreadId());
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadKillRaisesSigChldAgainstParentId)
{
    uint32 parent = threadCreate("chldParent", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);
    uint32 child = threadCreate("chldVictim", noopThreadEntry);
    KT_NE(t, child, threadInvalidId);

    threadAt(child)->parentId = parent;

    KT_EQU(t, threadAt(parent)->sigPending & (1ull << sigChld), 0ull);
    KT_TRUE(t, threadKill(child));
    KT_EQU(t, threadAt(parent)->sigPending & (1ull << sigChld), (1ull << sigChld));

    KT_TRUE(t, threadReap(child));
    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadKillEarlyWakesAParentGenuinelyBlockedWaitingForSigChld)
{
    int channel;

    uint32 parent = threadCreate("chldBlockedParent", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);
    uint32 child = threadCreate("chldVictim2", noopThreadEntry);
    KT_NE(t, child, threadInvalidId);
    threadAt(child)->parentId = parent;

    sigSetHandler(threadAt(parent), sigChld, 0x40005000ull, 0, 0);

    threadAt(parent)->state = threadBlocked;
    threadAt(parent)->waitChannel = &channel;

    KT_TRUE(t, threadKill(child));
    KT_EQ(t, threadAt(parent)->state, threadRunnable);

    KT_TRUE(t, threadReap(child));
    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadKillAgainstAnOrphanWithADeadParentIdRaisesNothing)
{
    uint32 deadParent = threadCreate("deadParent", noopThreadEntry);
    KT_NE(t, deadParent, threadInvalidId);
    KT_TRUE(t, threadKill(deadParent));
    KT_EQ(t, threadAt(deadParent)->state, threadDead);

    uint32 orphan = threadCreate("orphan", noopThreadEntry);
    KT_NE(t, orphan, threadInvalidId);
    threadAt(orphan)->parentId = deadParent;

    KT_TRUE(t, threadKill(orphan));
    KT_EQ(t, threadAt(orphan)->state, threadDead);
    KT_EQU(t, threadAt(deadParent)->sigPending & (1ull << sigChld), 0ull);

    KT_TRUE(t, threadReap(deadParent));
    KT_TRUE(t, threadReap(orphan));
}

KTEST(threadJoinAnyChildReturnsNoChildrenForAThreadWithNoneAtAll)
{
    uint32 parent = threadCreate("anyNoKids", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);

    uint32 outId = 0;
    int64 outCode = 0;
    KT_EQ(t, threadJoinAnyChild(parent, &outId, &outCode), waitScanNoChildren);

    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadJoinAnyChildReturnsNoneDeadWhileAllChildrenAreStillAlive)
{
    uint32 parent = threadCreate("anyLiveParent", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);
    uint32 child = threadCreate("anyLiveChild", noopThreadEntry);
    KT_NE(t, child, threadInvalidId);
    threadAt(child)->parentId = parent;

    uint32 outId = 0;
    int64 outCode = 0;
    KT_EQ(t, threadJoinAnyChild(parent, &outId, &outCode), waitScanNoneDead);
    KT_EQ(t, threadAt(child)->state, threadRunnable);

    KT_TRUE(t, threadDiscard(child));
    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadJoinAnyChildPicksTheLowestIndexedDeadChildAndCanBeCalledAgainForTheRest)
{
    uint32 parent = threadCreate("anyTwoKidsParent", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);
    uint32 childLow = threadCreate("anyChildLow", noopThreadEntry);
    KT_NE(t, childLow, threadInvalidId);
    uint32 childHigh = threadCreate("anyChildHigh", noopThreadEntry);
    KT_NE(t, childHigh, threadInvalidId);
    KT_LT(t, childLow, childHigh);
    threadAt(childLow)->parentId = parent;
    threadAt(childHigh)->parentId = parent;

    KT_TRUE(t, threadKill(childHigh));
    KT_TRUE(t, threadKill(childLow));

    uint32 outId = 0xFFFFFFFFu;
    int64 outCode = -1;
    KT_EQ(t, threadJoinAnyChild(parent, &outId, &outCode), waitScanFoundDead);
    KT_EQU(t, outId, childLow);
    KT_EQ(t, outCode, threadKilledExitCode);
    KT_EQ(t, threadAt(childLow)->state, threadUnused);
    KT_EQ(t, threadAt(childHigh)->state, threadDead);

    outId = 0xFFFFFFFFu;
    outCode = -1;
    KT_EQ(t, threadJoinAnyChild(parent, &outId, &outCode), waitScanFoundDead);
    KT_EQU(t, outId, childHigh);
    KT_EQ(t, outCode, threadKilledExitCode);

    KT_EQ(t, threadJoinAnyChild(parent, &outId, &outCode), waitScanNoChildren);

    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadKillWakesAParentGenuinelyBlockedInWaitAnyOnAnyChildDeath)
{
    uint32 parent = threadCreate("anyBlockedParent", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);
    uint32 child = threadCreate("anyBlockedChild", noopThreadEntry);
    KT_NE(t, child, threadInvalidId);
    threadAt(child)->parentId = parent;

    threadAt(parent)->state = threadBlocked;
    threadAt(parent)->waitChannel = threadAt(parent);

    KT_TRUE(t, threadKill(child));
    KT_EQ(t, threadAt(parent)->state, threadRunnable);

    KT_TRUE(t, threadReap(child));
    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadStopRejectsIdleOutOfRangeAndNeverCreatedSlots)
{
    KT_FALSE(t, threadStop(0));
    KT_FALSE(t, threadStop(threadCapacity()));
    KT_FALSE(t, threadStop(threadCapacity() + 1000));

    uint32 neverUsed = threadCount();
    if (neverUsed < threadCapacity())
        KT_FALSE(t, threadStop(neverUsed));
}

KTEST(threadStopMovesRunnableThreadToStoppedAndIsIdempotent)
{
    uint32 id = threadCreate("stopMe", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_EQ(t, threadAt(id)->state, threadRunnable);
    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    KT_TRUE(t, threadContinue(id));
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadStopAcceptsSleepingBlockedAndRunningStates)
{
    uint32 id = threadCreate("stopFromEveryState", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    threadAt(id)->state = threadSleeping;
    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);
    KT_TRUE(t, threadContinue(id));

    threadAt(id)->state = threadBlocked;
    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);
    KT_TRUE(t, threadContinue(id));

    threadAt(id)->state = threadRunning;
    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    KT_TRUE(t, threadContinue(id));
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadStopOnDeadOrUnusedSlotFails)
{
    uint32 id = threadCreate("stopDead", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    threadAt(id)->state = threadDead;
    KT_FALSE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadDead);
    KT_TRUE(t, threadReap(id));

    KT_FALSE(t, threadStop(id));
}

KTEST(threadContinueRejectsOutOfRangeAndNonStoppedThreads)
{
    KT_FALSE(t, threadContinue(threadCapacity()));
    KT_FALSE(t, threadContinue(threadCapacity() + 1000));

    uint32 id = threadCreate("notStopped", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_FALSE(t, threadContinue(id));
    KT_EQ(t, threadAt(id)->state, threadRunnable);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadStopExcludesTargetFromUnrelatedWaitChannelWake)
{
    int channel;

    uint32 id = threadCreate("stoppedWhileWaiting", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    threadAt(id)->state = threadBlocked;
    threadAt(id)->waitChannel = &channel;

    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    schedWakeChannel(&channel);
    KT_EQ(t, threadAt(id)->state, threadStopped);

    KT_TRUE(t, threadContinue(id));
    KT_EQ(t, threadAt(id)->state, threadRunnable);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadStopRaisesSigChldAgainstParentIdOnTheActualStopTransition)
{
    uint32 parent = threadCreate("stopChldParent", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);
    uint32 child = threadCreate("stopChldVictim", noopThreadEntry);
    KT_NE(t, child, threadInvalidId);
    threadAt(child)->parentId = parent;

    KT_EQU(t, threadAt(parent)->sigPending & (1ull << sigChld), 0ull);
    KT_TRUE(t, threadStop(child));
    KT_EQU(t, threadAt(parent)->sigPending & (1ull << sigChld), (1ull << sigChld));

    KT_TRUE(t, threadContinue(child));
    KT_TRUE(t, threadDiscard(child));
    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadStopDoesNotReRaiseSigChldAgainstAnAlreadyStoppedTarget)
{
    uint32 parent = threadCreate("stopChldParent2", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);
    uint32 child = threadCreate("stopChldVictim2", noopThreadEntry);
    KT_NE(t, child, threadInvalidId);
    threadAt(child)->parentId = parent;

    KT_TRUE(t, threadStop(child));
    KT_EQU(t, threadAt(parent)->sigPending & (1ull << sigChld), (1ull << sigChld));

    threadAt(parent)->sigPending &= ~(1ull << sigChld);

    KT_TRUE(t, threadStop(child));
    KT_EQ(t, threadAt(child)->state, threadStopped);
    KT_EQU(t, threadAt(parent)->sigPending & (1ull << sigChld), 0ull);

    KT_TRUE(t, threadContinue(child));
    KT_TRUE(t, threadDiscard(child));
    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadStopEarlyWakesAParentGenuinelyBlockedWaitingForSigChldOnStop)
{
    int channel;

    uint32 parent = threadCreate("stopChldBlockedParent", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);
    uint32 child = threadCreate("stopChldVictim3", noopThreadEntry);
    KT_NE(t, child, threadInvalidId);
    threadAt(child)->parentId = parent;

    sigSetHandler(threadAt(parent), sigChld, 0x40005000ull, 0, 0);

    threadAt(parent)->state = threadBlocked;
    threadAt(parent)->waitChannel = &channel;

    KT_TRUE(t, threadStop(child));
    KT_EQ(t, threadAt(parent)->state, threadRunnable);

    KT_TRUE(t, threadContinue(child));
    KT_TRUE(t, threadDiscard(child));
    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadContinueRaisesSigChldAgainstParentIdOnTheActualResumeTransition)
{
    uint32 parent = threadCreate("contChldParent", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);
    uint32 child = threadCreate("contChldVictim", noopThreadEntry);
    KT_NE(t, child, threadInvalidId);
    threadAt(child)->parentId = parent;

    KT_TRUE(t, threadStop(child));
    threadAt(parent)->sigPending &= ~(1ull << sigChld);

    KT_TRUE(t, threadContinue(child));
    KT_EQU(t, threadAt(parent)->sigPending & (1ull << sigChld), (1ull << sigChld));

    KT_TRUE(t, threadDiscard(child));
    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadContinueOnATargetThatWasNeverStoppedRaisesNothing)
{
    uint32 parent = threadCreate("contChldParent2", noopThreadEntry);
    KT_NE(t, parent, threadInvalidId);
    uint32 child = threadCreate("contChldVictim2", noopThreadEntry);
    KT_NE(t, child, threadInvalidId);
    threadAt(child)->parentId = parent;

    KT_EQ(t, threadAt(child)->state, threadRunnable);
    KT_FALSE(t, threadContinue(child));
    KT_EQU(t, threadAt(parent)->sigPending & (1ull << sigChld), 0ull);

    KT_TRUE(t, threadDiscard(child));
    KT_TRUE(t, threadDiscard(parent));
}

KTEST(threadStopAgainstAnOrphanWithADeadParentIdRaisesNothing)
{
    uint32 deadParent = threadCreate("stopDeadParent", noopThreadEntry);
    KT_NE(t, deadParent, threadInvalidId);
    KT_TRUE(t, threadKill(deadParent));
    KT_EQ(t, threadAt(deadParent)->state, threadDead);

    uint32 orphan = threadCreate("stopOrphan", noopThreadEntry);
    KT_NE(t, orphan, threadInvalidId);
    threadAt(orphan)->parentId = deadParent;

    KT_TRUE(t, threadStop(orphan));
    KT_EQ(t, threadAt(orphan)->state, threadStopped);
    KT_EQU(t, threadAt(deadParent)->sigPending & (1ull << sigChld), 0ull);

    KT_TRUE(t, threadContinue(orphan));
    KT_TRUE(t, threadReap(deadParent));
    KT_TRUE(t, threadDiscard(orphan));
}

KTEST(threadWakeForSignalRejectsIdleOutOfRangeAndNeverCreatedSlots)
{
    KT_FALSE(t, threadWakeForSignal(0));
    KT_FALSE(t, threadWakeForSignal(threadCapacity()));
    KT_FALSE(t, threadWakeForSignal(threadCapacity() + 1000));

    uint32 neverUsed = threadCount();
    if (neverUsed < threadCapacity())
        KT_FALSE(t, threadWakeForSignal(neverUsed));
}

KTEST(threadWakeForSignalMovesSleepingOrBlockedThreadToRunnable)
{
    uint32 id = threadCreate("wakeFromSleep", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    threadAt(id)->state = threadSleeping;
    KT_TRUE(t, threadWakeForSignal(id));
    KT_EQ(t, threadAt(id)->state, threadRunnable);

    threadAt(id)->state = threadBlocked;
    KT_TRUE(t, threadWakeForSignal(id));
    KT_EQ(t, threadAt(id)->state, threadRunnable);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadWakeForSignalIsNoOpOnRunnableRunningStoppedDeadAndUnused)
{
    uint32 id = threadCreate("wakeNoOp", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_EQ(t, threadAt(id)->state, threadRunnable);
    KT_FALSE(t, threadWakeForSignal(id));
    KT_EQ(t, threadAt(id)->state, threadRunnable);

    threadAt(id)->state = threadRunning;
    KT_FALSE(t, threadWakeForSignal(id));
    KT_EQ(t, threadAt(id)->state, threadRunning);

    KT_TRUE(t, threadStop(id));
    KT_FALSE(t, threadWakeForSignal(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);
    KT_TRUE(t, threadContinue(id));

    threadAt(id)->state = threadDead;
    KT_FALSE(t, threadWakeForSignal(id));
    KT_TRUE(t, threadReap(id));

    KT_FALSE(t, threadWakeForSignal(id));
}

KTEST(threadKillAcceptsStoppedThreadLikeRealSigkillOnAStoppedProcess)
{
    uint32 id = threadCreate("killStopped", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    KT_TRUE(t, threadKill(id));
    KT_EQ(t, threadAt(id)->state, threadDead);
    KT_EQU(t, threadAt(id)->exitCode, threadKilledExitCode);

    KT_TRUE(t, threadReap(id));
}

KTEST(threadDiscardClosesOpenReadPipeFdSoWriterSeesBrokenPipe)
{
    int handle = pipeCreateDetached();
    KT_GE(t, handle, 0);

    uint32 id = threadCreate("pipeReadHolder", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    pipeAddReader(handle);
    threadAt(id)->fds[3].kind = fdPipeRead;
    threadAt(id)->fds[3].pipeHandle = handle;

    pipeAddWriter(handle);
    KT_FALSE(t, pipeWriteBroken(handle));

    KT_TRUE(t, threadDiscard(id));

    KT_TRUE(t, pipeWriteBroken(handle));
    KT_EQ(t, pipeWrite(handle, "x", 1), -1);

    pipeCloseWrite(handle);
}

KTEST(threadReapClosesOpenWritePipeFdSoReaderSeesEof)
{
    int handle = pipeCreateDetached();
    KT_GE(t, handle, 0);

    uint32 id = threadCreate("pipeWriteHolder", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    pipeAddWriter(handle);
    threadAt(id)->fds[3].kind = fdPipeWrite;
    threadAt(id)->fds[3].pipeHandle = handle;

    pipeAddReader(handle);
    char buffer[4];
    KT_EQ(t, pipeRead(handle, buffer, sizeof(buffer)), -1);

    threadAt(id)->state = threadDead;
    KT_TRUE(t, threadReap(id));

    KT_EQ(t, pipeRead(handle, buffer, sizeof(buffer)), 0);

    pipeCloseRead(handle);
}

KTEST(threadSetAlarmReturnsZeroWithNoPriorAlarmAndRecordsAFutureDeadline)
{
    Thread* self = threadAt(currentThreadId());
    KT_EQU(t, self->alarmAt, 0ull);

    uint64 now = pitTicks();
    KT_EQU(t, threadSetAlarm(5), 0ull);
    KT_TRUE(t, self->alarmAt > now);
    KT_EQU(t, self->alarmAt, now + 5ull * pitFrequency());

    threadSetAlarm(0);
    KT_EQU(t, self->alarmAt, 0ull);
}

KTEST(threadSetAlarmReplacingOrCancelingAPendingAlarmReturnsItsRemainingSecondsRoundedUp)
{
    Thread* self = threadAt(currentThreadId());

    KT_EQU(t, threadSetAlarm(10), 0ull);
    KT_EQU(t, threadSetAlarm(3), 10ull);

    KT_EQU(t, threadSetAlarm(0), 3ull);
    KT_EQU(t, self->alarmAt, 0ull);

    KT_EQU(t, threadSetAlarm(0), 0ull);
}

KTEST(threadCreateDefaultsPgidToItsOwnIdWhenCurrentIsTheIdleThread)
{
    uint32 id = threadCreate("pgidDefaultKernel", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadAt(id)->pgid, id);
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadCreateUserDefaultsPgidToItsOwnIdWhenCurrentIsTheIdleThread)
{
    uint64 entry = 0x350000000ull + 0x2000;
    uint32 id = threadCreateUser("pgidDefaultUser", entry, elfSlotStackTop(entry));
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadAt(id)->pgid, id);
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadCreateDefaultsUidToRootWhenCurrentIsTheIdleThread)
{
    uint32 id = threadCreate("uidDefaultKernel", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadAt(id)->uid, 0u);
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadCreateUserDefaultsUidToRootWhenCurrentIsTheIdleThread)
{
    uint64 entry = 0x350000000ull + 0x2000;
    uint32 id = threadCreateUser("uidDefaultUser", entry, elfSlotStackTop(entry));
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadAt(id)->uid, 0u);
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadPgidReturnsInvalidForOutOfRangeOrNeverCreatedSlots)
{
    KT_EQU(t, threadPgid(threadCapacity()), threadInvalidId);
    KT_EQU(t, threadPgid(threadCapacity() + 1000), threadInvalidId);

    uint32 neverUsed = threadCount();
    if (neverUsed < threadCapacity())
        KT_EQU(t, threadPgid(neverUsed), threadInvalidId);
}

KTEST(threadPgidReturnsInvalidAfterDiscardOrReap)
{
    uint32 id = threadCreate("pgidGoneAfterDiscard", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadPgid(id), id);

    KT_TRUE(t, threadDiscard(id));
    KT_EQU(t, threadPgid(id), threadInvalidId);
}

KTEST(threadSetPgidRejectsIdleOutOfRangeAndNeverCreatedSlots)
{
    KT_FALSE(t, threadSetPgid(0, 1));
    KT_FALSE(t, threadSetPgid(threadCapacity(), 1));
    KT_FALSE(t, threadSetPgid(threadCapacity() + 1000, 1));

    uint32 neverUsed = threadCount();
    if (neverUsed < threadCapacity())
        KT_FALSE(t, threadSetPgid(neverUsed, 1));
}

KTEST(threadSetPgidChangesGroupOfALiveThread)
{
    uint32 leader = threadCreate("pgidLeader", noopThreadEntry);
    uint32 joiner = threadCreate("pgidJoiner", noopThreadEntry);
    KT_NE(t, leader, threadInvalidId);
    KT_NE(t, joiner, threadInvalidId);

    KT_NE(t, threadAt(joiner)->pgid, threadAt(leader)->pgid);
    KT_TRUE(t, threadSetPgid(joiner, threadAt(leader)->pgid));
    KT_EQU(t, threadAt(joiner)->pgid, threadAt(leader)->pgid);

    KT_TRUE(t, threadDiscard(leader));
    KT_TRUE(t, threadDiscard(joiner));
}

KTEST(threadSetPgidRejectsDeadOrUnusedTarget)
{
    uint32 id = threadCreate("pgidSetOnDead", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, threadKill(id));
    KT_FALSE(t, threadSetPgid(id, 12345));

    KT_TRUE(t, threadReap(id));
    KT_FALSE(t, threadSetPgid(id, 12345));
}

KTEST(threadPriorityDefaultsToSchedDefaultWeightForANewThread)
{
    uint32 id = threadCreate("priorityDefault", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadPriority(id), schedDefaultWeight);
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadPriorityReturnsZeroForOutOfRangeOrNeverCreatedSlots)
{
    KT_EQU(t, threadPriority(threadCapacity()), 0u);
    KT_EQU(t, threadPriority(threadCapacity() + 1000), 0u);

    uint32 neverUsed = threadCount();
    if (neverUsed < threadCapacity())
        KT_EQU(t, threadPriority(neverUsed), 0u);
}

KTEST(threadPriorityReturnsZeroAfterDiscard)
{
    uint32 id = threadCreate("priorityGoneAfterDiscard", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadPriority(id), schedDefaultWeight);

    KT_TRUE(t, threadDiscard(id));
    KT_EQU(t, threadPriority(id), 0u);
}

KTEST(threadSetPriorityRejectsIdleOutOfRangeZeroWeightAndNeverCreatedSlots)
{
    KT_FALSE(t, threadSetPriority(0, 2048));
    KT_FALSE(t, threadSetPriority(threadCapacity(), 2048));
    KT_FALSE(t, threadSetPriority(threadCapacity() + 1000, 2048));

    uint32 id = threadCreate("priorityRejectZero", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_FALSE(t, threadSetPriority(id, 0));
    KT_EQU(t, threadPriority(id), schedDefaultWeight);
    KT_TRUE(t, threadDiscard(id));

    uint32 neverUsed = threadCount();
    if (neverUsed < threadCapacity())
        KT_FALSE(t, threadSetPriority(neverUsed, 2048));
}

KTEST(threadSetPriorityChangesWeightOfALiveThread)
{
    uint32 id = threadCreate("priorityChange", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, threadSetPriority(id, 4096));
    KT_EQU(t, threadPriority(id), 4096u);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadSetPriorityRejectsDeadOrUnusedTarget)
{
    uint32 id = threadCreate("prioritySetOnDead", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, threadKill(id));
    KT_FALSE(t, threadSetPriority(id, 4096));

    KT_TRUE(t, threadReap(id));
    KT_FALSE(t, threadSetPriority(id, 4096));
}

KTEST(threadKillGroupKillsEveryLiveMemberAndLeavesOtherGroupsAlone)
{
    uint32 a = threadCreate("groupKillA", noopThreadEntry);
    uint32 b = threadCreate("groupKillB", noopThreadEntry);
    uint32 outsider = threadCreate("groupKillOutsider", noopThreadEntry);
    KT_NE(t, a, threadInvalidId);
    KT_NE(t, b, threadInvalidId);
    KT_NE(t, outsider, threadInvalidId);

    uint32 pgid = threadAt(a)->pgid;
    KT_TRUE(t, threadSetPgid(b, pgid));
    KT_NE(t, threadAt(outsider)->pgid, pgid);

    KT_TRUE(t, threadKillGroup(pgid));
    KT_EQ(t, threadAt(a)->state, threadDead);
    KT_EQ(t, threadAt(b)->state, threadDead);
    KT_EQ(t, threadAt(outsider)->state, threadRunnable);

    KT_TRUE(t, threadReap(a));
    KT_TRUE(t, threadReap(b));
    KT_TRUE(t, threadDiscard(outsider));
}

KTEST(threadKillGroupReturnsFalseWhenNoLiveThreadHasThatPgid)
{
    KT_FALSE(t, threadKillGroup(0xBEEF0000u));
}

KTEST(threadKillGroupSkipsAlreadyDeadMembersButStillReportsTheGroupAsFound)
{
    uint32 a = threadCreate("groupKillDeadA", noopThreadEntry);
    uint32 b = threadCreate("groupKillDeadB", noopThreadEntry);
    KT_NE(t, a, threadInvalidId);
    KT_NE(t, b, threadInvalidId);

    uint32 pgid = threadAt(a)->pgid;
    KT_TRUE(t, threadSetPgid(b, pgid));

    KT_TRUE(t, threadKill(b));
    KT_TRUE(t, threadKillGroup(pgid));
    KT_EQ(t, threadAt(a)->state, threadDead);

    KT_TRUE(t, threadReap(a));
    KT_TRUE(t, threadReap(b));
}

KTEST(currentThreadSetEnvThenGetEnvRoundTrips)
{
    threadAt(0)->envCount = 0;

    KT_TRUE(t, currentThreadSetEnv("FOO", "bar"));
    KT_STREQ(t, currentThreadGetEnv("FOO"), "bar");
    KT_NULL(t, currentThreadGetEnv("MISSING"));
}

KTEST(currentThreadSetEnvOverwritesExistingNameInPlace)
{
    threadAt(0)->envCount = 0;

    KT_TRUE(t, currentThreadSetEnv("FOO", "bar"));
    KT_TRUE(t, currentThreadSetEnv("FOO", "baz"));
    KT_STREQ(t, currentThreadGetEnv("FOO"), "baz");
    KT_EQ(t, currentThreadEnvCount(), 1);
}

KTEST(currentThreadSetEnvRejectsEntryTooLongForStorage)
{
    threadAt(0)->envCount = 0;

    char hugeValue[envEntryMax];
    for (int i = 0; i < envEntryMax - 1; i++)
        hugeValue[i] = 'x';
    hugeValue[envEntryMax - 1] = 0;

    KT_FALSE(t, currentThreadSetEnv("FOO", hugeValue));
    KT_NULL(t, currentThreadGetEnv("FOO"));
    KT_EQ(t, currentThreadEnvCount(), 0);
}

KTEST(currentThreadSetEnvRejectsNewNameOnceTableIsFull)
{
    threadAt(0)->envCount = 0;

    char name[8];
    for (int i = 0; i < envMaxVars; i++) {
        snprintf(name, sizeof(name), "V%d", i);
        KT_TRUE(t, currentThreadSetEnv(name, "x"));
    }
    KT_EQ(t, currentThreadEnvCount(), envMaxVars);

    KT_FALSE(t, currentThreadSetEnv("ONE_TOO_MANY", "x"));
    KT_EQ(t, currentThreadEnvCount(), envMaxVars);

    KT_TRUE(t, currentThreadSetEnv("V0", "changed"));
    KT_STREQ(t, currentThreadGetEnv("V0"), "changed");
    KT_EQ(t, currentThreadEnvCount(), envMaxVars);

    threadAt(0)->envCount = 0;
}

KTEST(currentThreadEnvEntryExposesRawStorageForListing)
{
    threadAt(0)->envCount = 0;

    KT_TRUE(t, currentThreadSetEnv("A", "1"));
    KT_TRUE(t, currentThreadSetEnv("B", "2"));
    KT_EQ(t, currentThreadEnvCount(), 2);
    KT_STREQ(t, currentThreadEnvEntry(0), "A=1");
    KT_STREQ(t, currentThreadEnvEntry(1), "B=2");
    KT_NULL(t, currentThreadEnvEntry(2));
    KT_NULL(t, currentThreadEnvEntry(-1));

    threadAt(0)->envCount = 0;
}

KTEST(currentThreadUnsetEnvOnMissingNameIsNoOpSuccess)
{
    threadAt(0)->envCount = 0;

    KT_TRUE(t, currentThreadSetEnv("FOO", "bar"));
    KT_FALSE(t, currentThreadUnsetEnv("MISSING"));
    KT_EQ(t, currentThreadEnvCount(), 1);
    KT_STREQ(t, currentThreadGetEnv("FOO"), "bar");

    threadAt(0)->envCount = 0;
}

KTEST(currentThreadUnsetEnvRemovesLastEntry)
{
    threadAt(0)->envCount = 0;

    KT_TRUE(t, currentThreadSetEnv("A", "1"));
    KT_TRUE(t, currentThreadSetEnv("B", "2"));
    KT_TRUE(t, currentThreadUnsetEnv("B"));
    KT_EQ(t, currentThreadEnvCount(), 1);
    KT_STREQ(t, currentThreadGetEnv("A"), "1");
    KT_NULL(t, currentThreadGetEnv("B"));

    threadAt(0)->envCount = 0;
}

KTEST(currentThreadUnsetEnvCompactsMiddleEntryDown)
{
    threadAt(0)->envCount = 0;

    KT_TRUE(t, currentThreadSetEnv("A", "1"));
    KT_TRUE(t, currentThreadSetEnv("B", "2"));
    KT_TRUE(t, currentThreadSetEnv("C", "3"));
    KT_TRUE(t, currentThreadUnsetEnv("B"));

    KT_EQ(t, currentThreadEnvCount(), 2);
    KT_STREQ(t, currentThreadEnvEntry(0), "A=1");
    KT_STREQ(t, currentThreadEnvEntry(1), "C=3");
    KT_NULL(t, currentThreadEnvEntry(2));
    KT_NULL(t, currentThreadGetEnv("B"));
    KT_STREQ(t, currentThreadGetEnv("A"), "1");
    KT_STREQ(t, currentThreadGetEnv("C"), "3");

    threadAt(0)->envCount = 0;
}

KTEST(currentThreadUnsetEnvActuallyFreesSlotForReuse)
{
    threadAt(0)->envCount = 0;

    for (int i = 0; i < envMaxVars; i++) {
        char name[8];
        snprintf(name, sizeof(name), "V%d", i);
        KT_TRUE(t, currentThreadSetEnv(name, "x"));
    }
    KT_EQ(t, currentThreadEnvCount(), envMaxVars);

    KT_TRUE(t, currentThreadUnsetEnv("V0"));
    KT_EQ(t, currentThreadEnvCount(), envMaxVars - 1);

    KT_TRUE(t, currentThreadSetEnv("FRESH", "new"));
    KT_EQ(t, currentThreadEnvCount(), envMaxVars);
    KT_STREQ(t, currentThreadGetEnv("FRESH"), "new");
    KT_NULL(t, currentThreadGetEnv("V0"));

    threadAt(0)->envCount = 0;
}
