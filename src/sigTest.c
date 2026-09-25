#include "ktest.h"
#include "sched.h"
#include "signal.h"
#include "ports.h"
#include "string.h"
#include "gdt.h"

static void noopThreadEntry(void)
{
    for (;;)
        halt();
}

static Registers makeUserRegs(void)
{
    Registers regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip = 0x40001234ull;
    regs.rax = 0xAAAAAAAAull;
    regs.rdi = 0xBBBBBBBBull;
    regs.rbx = 0xCCCCCCCCull;
    regs.rsp = 0x48003FF0ull;
    regs.cs = userCodeSelector | 3;
    regs.ss = userDataSelector | 3;
    regs.rflags = 0x202;
    return regs;
}

KTEST(sigRaiseSetsPendingBitAndIsIdempotent)
{
    uint32 id = threadCreate("sigRaiseTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, sigRaise(id, sigUsr1));
    KT_EQU(t, threadAt(id)->sigPending, (1ull << sigUsr1));

    KT_TRUE(t, sigRaise(id, sigUsr1));
    KT_EQU(t, threadAt(id)->sigPending, (1ull << sigUsr1));

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigRaiseRejectsOutOfRangeSignumAndBadTarget)
{
    uint32 id = threadCreate("sigRaiseBad", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_FALSE(t, sigRaise(id, 0));
    KT_FALSE(t, sigRaise(id, -1));
    KT_FALSE(t, sigRaise(id, sigMaxSignals));

    KT_FALSE(t, sigRaise(0, sigUsr1));
    KT_FALSE(t, sigRaise(threadCapacity(), sigUsr1));
    KT_FALSE(t, sigRaise(threadCapacity() + 1000, sigUsr1));

    KT_TRUE(t, threadDiscard(id));

    KT_FALSE(t, sigRaise(id, sigUsr1));
}

KTEST(sigRaiseSigKillDelegatesToThreadKill)
{
    uint32 id = threadCreate("sigKillTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, sigRaise(id, sigKill));
    KT_EQ(t, threadAt(id)->state, threadDead);
    KT_EQU(t, threadAt(id)->exitCode, threadKilledExitCode);

    KT_EQU(t, threadAt(id)->sigPending, 0ull);

    KT_TRUE(t, threadReap(id));
}

KTEST(sigRaiseSigStopDelegatesToThreadStopAndBypassesPending)
{
    uint32 id = threadCreate("sigStopTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, sigRaise(id, sigStop));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    KT_EQU(t, threadAt(id)->sigPending, 0ull);

    KT_TRUE(t, threadKill(id));
    KT_TRUE(t, threadReap(id));
}

KTEST(sigRaiseSigStopRejectsInvalidTargetsLikeThreadStop)
{
    KT_FALSE(t, sigRaise(0, sigStop));
    KT_FALSE(t, sigRaise(threadCapacity(), sigStop));
    KT_FALSE(t, sigRaise(threadCapacity() + 1000, sigStop));
}

KTEST(sigRaiseSigContResumesStoppedTargetUnconditionally)
{
    uint32 id = threadCreate("sigContTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, sigRaise(id, sigStop));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    sigProcMask(threadAt(id), sigMaskSetMask, sigAllBlocked);
    KT_TRUE(t, sigRaise(id, sigCont));
    KT_EQ(t, threadAt(id)->state, threadRunnable);
    KT_TRUE(t, threadAt(id)->sigPending & (1ull << sigCont));

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigRaiseSigContOnNonStoppedTargetStillDeliversSignal)
{
    uint32 id = threadCreate("sigContNotStopped", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, sigRaise(id, sigCont));
    KT_EQ(t, threadAt(id)->state, threadRunnable);
    KT_TRUE(t, threadAt(id)->sigPending & (1ull << sigCont));

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigRaiseWakesSleepingOrBlockedTargetEarlyForADeliverableSignal)
{
    uint32 id = threadCreate("sigWakeSleeping", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    thread->state = threadSleeping;
    KT_TRUE(t, sigRaise(id, sigTerm));
    KT_EQ(t, thread->state, threadRunnable);

    thread->state = threadBlocked;
    KT_TRUE(t, sigRaise(id, sigTstp));
    KT_EQ(t, thread->state, threadRunnable);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigRaiseDoesNotWakeSleepingOrBlockedTargetForABlockedOrIgnoredSignal)
{
    uint32 id = threadCreate("sigNoWakeBlocked", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    sigProcMask(thread, sigMaskBlock, (1ull << sigUsr1));
    thread->state = threadSleeping;
    KT_TRUE(t, sigRaise(id, sigUsr1));
    KT_EQ(t, thread->state, threadSleeping);
    KT_TRUE(t, thread->sigPending & (1ull << sigUsr1));

    thread->state = threadBlocked;
    KT_TRUE(t, sigRaise(id, sigChld));
    KT_EQ(t, thread->state, threadBlocked);

    sigSetHandler(thread, sigUsr2, sigHandlerIgnore, 0, 0);
    KT_TRUE(t, sigRaise(id, sigUsr2));
    KT_EQ(t, thread->state, threadBlocked);

    threadAt(id)->state = threadRunnable;
    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigRaiseDoesNotEarlyWakeForSignalMaskedByActiveHandlersSaMask)
{
    uint32 id = threadCreate("sigMaskedNoEarlyWake", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    int signum = -1;

    uint64 usr1Handler = 0x40009000ull;
    sigSetHandler(thread, sigUsr1, usr1Handler, (1ull << sigHup), 0);
    sigRaise(id, sigUsr1);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_TRUE(t, thread->sigBlocked & (1ull << sigHup));

    thread->state = threadBlocked;

    KT_TRUE(t, sigRaise(id, sigHup));
    KT_EQ(t, thread->state, threadBlocked);
    KT_TRUE(t, thread->sigPending & (1ull << sigHup));

    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_FALSE(t, thread->sigBlocked & (1ull << sigHup));
    KT_EQ(t, thread->state, threadBlocked);

    threadAt(id)->state = threadRunnable;
    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigRaiseNeverWakesAStoppedTargetForAnOrdinarySignal)
{
    uint32 id = threadCreate("sigStoppedNoWake", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    KT_TRUE(t, sigRaise(id, sigTerm));
    KT_EQ(t, threadAt(id)->state, threadStopped);
    KT_TRUE(t, threadAt(id)->sigPending & (1ull << sigTerm));

    KT_TRUE(t, threadKill(id));
    KT_TRUE(t, threadReap(id));
}

KTEST(sigSetHandlerInstallsAndReturnsPreviousDisposition)
{
    uint32 id = threadCreate("sigHandlerTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    KT_EQU(t, sigSetHandler(thread, sigUsr1, 0x40005000ull, 0, 0), sigHandlerDefault);
    KT_EQU(t, thread->sigHandlers[sigUsr1], 0x40005000ull);

    KT_EQU(t, sigSetHandler(thread, sigUsr1, sigHandlerIgnore, 0, 0), 0x40005000ull);
    KT_EQU(t, thread->sigHandlers[sigUsr1], sigHandlerIgnore);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigSetHandlerRejectsSigKillAndOutOfRangeSignum)
{
    uint32 id = threadCreate("sigHandlerBad", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    KT_EQU(t, sigSetHandler(thread, sigKill, 0x40005000ull, 0, 0), sigSetFailed);
    KT_EQU(t, thread->sigHandlers[sigKill], sigHandlerDefault);

    KT_EQU(t, sigSetHandler(thread, 0, 0x40005000ull, 0, 0), sigSetFailed);
    KT_EQU(t, sigSetHandler(thread, sigMaxSignals, 0x40005000ull, 0, 0), sigSetFailed);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigSetHandlerRejectsSigStopTooLikeRealEinval)
{
    uint32 id = threadCreate("sigStopHandlerBad", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    KT_EQU(t, sigSetHandler(thread, sigStop, 0x40005000ull, 0, 0), sigSetFailed);
    KT_EQU(t, thread->sigHandlers[sigStop], sigHandlerDefault);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigSetHandlerStoresMaskStrippedOfSigKillAndSigStop)
{
    uint32 id = threadCreate("sigMaskStoreTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    sigSetHandler(thread, sigUsr1, 0x40005000ull,
                  (1ull << sigUsr2) | (1ull << sigKill) | (1ull << sigStop), 0);
    KT_EQU(t, thread->sigHandlerMasks[sigUsr1], (1ull << sigUsr2));

    sigSetHandler(thread, sigTerm, 0x40006000ull, (1ull << sigHup), 0);
    KT_EQU(t, thread->sigHandlerMasks[sigUsr1], (1ull << sigUsr2));
    KT_EQU(t, thread->sigHandlerMasks[sigTerm], (1ull << sigHup));

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigProcMaskBlockUnblockSetMaskReturnPreviousAndStripSigKill)
{
    uint32 id = threadCreate("sigMaskTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    KT_EQU(t, sigProcMask(thread, sigMaskBlock, (1ull << sigUsr1)), 0ull);
    KT_EQU(t, thread->sigBlocked, (1ull << sigUsr1));

    KT_EQU(t, sigProcMask(thread, sigMaskBlock, (1ull << sigUsr2)),
           (1ull << sigUsr1));
    KT_EQU(t, thread->sigBlocked, (1ull << sigUsr1) | (1ull << sigUsr2));

    KT_EQU(t, sigProcMask(thread, sigMaskUnblock, (1ull << sigUsr1)),
           (1ull << sigUsr1) | (1ull << sigUsr2));
    KT_EQU(t, thread->sigBlocked, (1ull << sigUsr2));

    KT_EQU(t, sigProcMask(thread, sigMaskSetMask, (1ull << sigTerm)),
           (1ull << sigUsr2));
    KT_EQU(t, thread->sigBlocked, (1ull << sigTerm));

    KT_EQU(t, sigProcMask(thread, sigMaskSetMask, sigAllBlocked), (1ull << sigTerm));
    KT_FALSE(t, thread->sigBlocked & (1ull << sigKill));
    KT_FALSE(t, thread->sigBlocked & (1ull << sigStop));

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingReturnsNoneWhenNothingIsPendingOrEverythingIsBlocked)
{
    uint32 id = threadCreate("sigConsumeNone", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    int signum = -1;

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeNone);
    KT_EQ(t, signum, -1);

    sigRaise(id, sigUsr1);
    sigProcMask(thread, sigMaskBlock, (1ull << sigUsr1));
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeNone);
    KT_TRUE(t, thread->sigPending & (1ull << sigUsr1));

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingDismissesIgnoredSignalsAndKeepsScanning)
{
    uint32 id = threadCreate("sigConsumeIgnore", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    int signum = -1;

    sigRaise(id, sigChld);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeNone);
    KT_FALSE(t, thread->sigPending & (1ull << sigChld));

    sigSetHandler(thread, sigUsr1, sigHandlerIgnore, 0, 0);
    sigRaise(id, sigUsr1);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeNone);
    KT_FALSE(t, thread->sigPending & (1ull << sigUsr1));

    sigSetHandler(thread, sigHup, sigHandlerIgnore, 0, 0);
    sigRaise(id, sigHup);
    sigRaise(id, sigTerm);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeTerminate);
    KT_EQ(t, signum, sigTerm);
    KT_FALSE(t, thread->sigPending & (1ull << sigHup));
    KT_FALSE(t, thread->sigPending & (1ull << sigTerm));

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingDefaultDispositionReturnsTerminate)
{
    uint32 id = threadCreate("sigConsumeTerm", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    Registers before = regs;
    int signum = -1;

    sigRaise(id, sigTerm);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeTerminate);
    KT_EQ(t, signum, sigTerm);
    KT_FALSE(t, thread->sigPending & (1ull << sigTerm));

    KT_EQU(t, regs.rip, before.rip);
    KT_EQU(t, regs.rax, before.rax);
    KT_EQU(t, thread->sigHandlerDepth, 0u);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingSigTstpDefaultDispositionReturnsStopOutcome)
{
    uint32 id = threadCreate("sigConsumeTstp", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    Registers before = regs;
    int signum = -1;

    sigRaise(id, sigTstp);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeStop);
    KT_EQ(t, signum, sigTstp);
    KT_FALSE(t, thread->sigPending & (1ull << sigTstp));

    KT_EQU(t, regs.rip, before.rip);
    KT_EQU(t, regs.rax, before.rax);
    KT_EQU(t, thread->sigHandlerDepth, 0u);
    KT_EQ(t, thread->state, threadRunnable);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingSigTstpWithHandlerRunsHandlerInsteadOfStopping)
{
    uint32 id = threadCreate("sigConsumeTstpCaught", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    int signum = -1;

    uint64 handlerAddr = 0x4000A000ull;
    sigSetHandler(thread, sigTstp, handlerAddr, 0, 0);
    sigRaise(id, sigTstp);

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_EQ(t, signum, sigTstp);
    KT_EQU(t, regs.rax, handlerAddr);
    KT_EQU(t, regs.rip, sigTrampolineVirtualAddr);
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingSaMaskDefersDifferentSignalUntilHandlerReturns)
{
    uint32 id = threadCreate("sigSaMaskTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    int signum = -1;

    uint64 usr1Handler = 0x40009000ull;
    uint64 hupHandler = 0x4000C000ull;
    sigSetHandler(thread, sigUsr1, usr1Handler, (1ull << sigHup), 0);
    sigSetHandler(thread, sigHup, hupHandler, 0, 0);

    sigRaise(id, sigUsr1);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_EQ(t, signum, sigUsr1);
    KT_TRUE(t, thread->sigBlocked & (1ull << sigHup));

    sigRaise(id, sigHup);
    int deferredSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &deferredSignum), sigOutcomeNone);
    KT_EQ(t, deferredSignum, -1);
    KT_TRUE(t, thread->sigPending & (1ull << sigHup));
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_FALSE(t, thread->sigBlocked & (1ull << sigHup));

    int finalSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &finalSignum), sigOutcomeHandler);
    KT_EQ(t, finalSignum, sigHup);
    KT_EQU(t, regs.rax, hupHandler);
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingDefersSameSignalRaisedAgainWhileItsOwnHandlerIsActive)
{
    uint32 id = threadCreate("sigNoDeferOffTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    int signum = -1;

    uint64 handlerAddr = 0x40009000ull;
    sigSetHandler(thread, sigUsr1, handlerAddr, 0, 0);
    sigRaise(id, sigUsr1);

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_EQ(t, signum, sigUsr1);
    KT_TRUE(t, thread->sigBlocked & (1ull << sigUsr1));
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    sigRaise(id, sigUsr1);
    int deferredSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &deferredSignum), sigOutcomeNone);
    KT_EQ(t, deferredSignum, -1);
    KT_TRUE(t, thread->sigPending & (1ull << sigUsr1));
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_FALSE(t, thread->sigBlocked & (1ull << sigUsr1));

    int finalSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &finalSignum), sigOutcomeHandler);
    KT_EQ(t, finalSignum, sigUsr1);
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingNoDeferFlagLetsSameSignalNestIntoItsOwnActiveHandler)
{
    uint32 id = threadCreate("sigNoDeferOnTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    int signum = -1;

    uint64 handlerAddr = 0x40009000ull;
    sigSetHandler(thread, sigUsr1, handlerAddr, 0, sigFlagNoDefer);
    sigRaise(id, sigUsr1);

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_EQ(t, signum, sigUsr1);
    KT_FALSE(t, thread->sigBlocked & (1ull << sigUsr1));
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    Registers midHandler = regs;
    sigRaise(id, sigUsr1);
    int nestedSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &nestedSignum), sigOutcomeHandler);
    KT_EQ(t, nestedSignum, sigUsr1);
    KT_EQU(t, regs.rip, sigTrampolineVirtualAddr);
    KT_EQU(t, regs.rax, handlerAddr);
    KT_EQU(t, thread->sigHandlerDepth, 2u);
    KT_FALSE(t, thread->sigBlocked & (1ull << sigUsr1));
    KT_EQ(t, memcmp(&thread->sigSavedRegs[1], &midHandler, sizeof(midHandler)), 0);

    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_EQ(t, memcmp(&regs, &midHandler, sizeof(midHandler)), 0);
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_EQU(t, thread->sigHandlerDepth, 0u);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingCustomHandlerRewritesTrapframeAndPushesContext)
{
    uint32 id = threadCreate("sigConsumeHandler", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    Registers before = regs;
    int signum = -1;

    uint64 handlerAddr = 0x40009000ull;
    sigSetHandler(thread, sigUsr1, handlerAddr, 0, 0);
    sigProcMask(thread, sigMaskBlock, (1ull << sigUsr2));
    uint64 blockedBefore = thread->sigBlocked;
    sigRaise(id, sigUsr1);

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_EQ(t, signum, sigUsr1);

    KT_EQU(t, regs.rip, sigTrampolineVirtualAddr);
    KT_EQU(t, regs.rax, handlerAddr);
    KT_EQU(t, regs.rdi, (uint64)sigUsr1);
    KT_EQU(t, regs.rsp, before.rsp);

    KT_EQU(t, thread->sigHandlerDepth, 1u);
    KT_EQU(t, thread->sigBlocked, blockedBefore | (1ull << sigUsr1));
    KT_EQU(t, thread->sigSavedBlocked[0], blockedBefore);

    KT_EQ(t, memcmp(&thread->sigSavedRegs[0], &before, sizeof(before)), 0);

    sigRaise(id, sigUsr2);
    int blockedSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &blockedSignum), sigOutcomeNone);
    KT_EQ(t, blockedSignum, -1);
    KT_TRUE(t, thread->sigPending & (1ull << sigUsr2));

    Registers midUsr1Handler = regs;
    uint64 handlerAddrHup = 0x4000B000ull;
    sigSetHandler(thread, sigHup, handlerAddrHup, 0, 0);
    sigRaise(id, sigHup);
    int nestedSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &nestedSignum), sigOutcomeHandler);
    KT_EQ(t, nestedSignum, sigHup);
    KT_EQU(t, regs.rip, sigTrampolineVirtualAddr);
    KT_EQU(t, regs.rax, handlerAddrHup);
    KT_EQU(t, regs.rdi, (uint64)sigHup);
    KT_EQU(t, thread->sigHandlerDepth, 2u);
    KT_EQU(t, thread->sigBlocked,
           blockedBefore | (1ull << sigUsr1) | (1ull << sigHup));
    KT_EQ(t, memcmp(&thread->sigSavedRegs[1], &midUsr1Handler,
                     sizeof(midUsr1Handler)), 0);
    KT_EQU(t, thread->sigSavedBlocked[1], blockedBefore | (1ull << sigUsr1));

    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_EQ(t, memcmp(&regs, &midUsr1Handler, sizeof(midUsr1Handler)), 0);
    KT_EQU(t, thread->sigHandlerDepth, 1u);
    KT_EQU(t, thread->sigBlocked, blockedBefore | (1ull << sigUsr1));

    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_EQ(t, memcmp(&regs, &before, sizeof(before)), 0);
    KT_EQU(t, thread->sigHandlerDepth, 0u);
    KT_EQU(t, thread->sigBlocked, blockedBefore);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingRefusesNewHandlerAtMaxNestDepth)
{
    uint32 id = threadCreate("sigNestDepthLimit", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    int signum = -1;

    thread->sigHandlerDepth = sigMaxNestDepth;
    sigSetHandler(thread, sigUsr1, 0x40009000ull, 0, 0);
    sigRaise(id, sigUsr1);

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeNone);
    KT_EQ(t, signum, -1);
    KT_TRUE(t, thread->sigPending & (1ull << sigUsr1));
    KT_EQU(t, thread->sigHandlerDepth, (uint32)sigMaxNestDepth);

    thread->sigHandlerDepth = 0;
    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingReturnsEintrForInterruptedRetryableSyscallWithoutRestartFlag)
{
    uint32 id = threadCreate("sigEintrTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    regs.rip = 0x40001232ull;
    int signum = -1;

    uint64 handlerAddr = 0x4000A000ull;
    sigSetHandler(thread, sigUsr1, handlerAddr, 0, 0);
    thread->sigInRetryableSyscall = true;
    sigRaise(id, sigUsr1);

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_EQ(t, signum, sigUsr1);

    KT_EQU(t, regs.rip, sigTrampolineVirtualAddr);
    KT_EQU(t, regs.rax, handlerAddr);

    KT_EQU(t, thread->sigSavedRegs[0].rip, 0x40001232ull + 2);
    KT_EQU(t, thread->sigSavedRegs[0].rax, (uint64)(int64)errInterrupted);

    KT_FALSE(t, thread->sigInRetryableSyscall);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingPreservesTransparentRetryWhenRestartFlagSet)
{
    uint32 id = threadCreate("sigRestartTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    regs.rip = 0x40001232ull;
    Registers before = regs;
    int signum = -1;

    sigSetHandler(thread, sigUsr1, 0x4000A000ull, 0, sigFlagRestart);
    thread->sigInRetryableSyscall = true;
    sigRaise(id, sigUsr1);

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);

    KT_EQ(t, memcmp(&thread->sigSavedRegs[0], &before, sizeof(before)), 0);
    KT_FALSE(t, thread->sigInRetryableSyscall);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingLeavesRegsAloneWhenNotInterruptingARetryableSyscall)
{
    uint32 id = threadCreate("sigNoRetryTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    Registers before = regs;
    int signum = -1;

    sigSetHandler(thread, sigUsr1, 0x4000A000ull, 0, 0);
    KT_FALSE(t, thread->sigInRetryableSyscall);
    sigRaise(id, sigUsr1);

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_EQ(t, memcmp(&thread->sigSavedRegs[0], &before, sizeof(before)), 0);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingNestedHandlerNeverInheritsOuterRetryState)
{
    uint32 id = threadCreate("sigNestedRetryTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    regs.rip = 0x40001232ull;
    int outerSignum = -1;

    sigSetHandler(thread, sigUsr1, 0x4000A000ull, 0, sigFlagRestart);
    sigSetHandler(thread, sigHup, 0x4000B000ull, 0, 0);
    thread->sigInRetryableSyscall = true;
    sigRaise(id, sigUsr1);
    KT_EQ(t, sigConsumePending(thread, &regs, &outerSignum), sigOutcomeHandler);
    KT_FALSE(t, thread->sigInRetryableSyscall);

    Registers midOuterHandler = regs;
    int nestedSignum = -1;
    sigRaise(id, sigHup);
    KT_EQ(t, sigConsumePending(thread, &regs, &nestedSignum), sigOutcomeHandler);

    KT_EQ(t, memcmp(&thread->sigSavedRegs[1], &midOuterHandler,
                     sizeof(midOuterHandler)), 0);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigOnSigReturnRestoresExactPreSignalContext)
{
    uint32 id = threadCreate("sigReturnTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    Registers before = regs;
    int signum = -1;

    sigSetHandler(thread, sigUsr1, 0x40009000ull, 0, 0);
    sigProcMask(thread, sigMaskBlock, (1ull << sigUsr2));
    uint64 blockedBefore = thread->sigBlocked;
    sigRaise(id, sigUsr1);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);

    regs.rax = 0xDEADBEEFull;
    regs.rdi = 0xFEEDFACEull;

    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_EQ(t, memcmp(&regs, &before, sizeof(before)), 0);
    KT_EQU(t, thread->sigHandlerDepth, 0u);
    KT_EQU(t, thread->sigBlocked, blockedBefore);

    Registers untouched = regs;
    KT_FALSE(t, sigOnSigReturn(thread, &regs));
    KT_EQ(t, memcmp(&regs, &untouched, sizeof(untouched)), 0);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadCreateResetsSignalStateOnRecycledSlot)
{
    uint32 first = threadCreate("sigLeakA", noopThreadEntry);
    KT_NE(t, first, threadInvalidId);
    Thread* a = threadAt(first);

    sigSetHandler(a, sigUsr1, 0x40009000ull, (1ull << sigUsr2), 0);
    sigProcMask(a, sigMaskBlock, (1ull << sigUsr2));
    sigRaise(first, sigUsr1);
    Registers regs = makeUserRegs();
    int signum = -1;
    KT_EQ(t, sigConsumePending(a, &regs, &signum), sigOutcomeHandler);
    KT_EQU(t, a->sigHandlerDepth, 1u);

    KT_TRUE(t, threadDiscard(first));

    uint32 second = threadInvalidId;
    for (uint32 attempt = 0; attempt < threadCapacity(); attempt++) {
        uint32 candidate = threadCreate("sigLeakB", noopThreadEntry);
        KT_NE(t, candidate, threadInvalidId);
        if (candidate == first) {
            second = candidate;
            break;
        }
        KT_TRUE(t, threadDiscard(candidate));
    }
    KT_EQ(t, second, first);
    Thread* b = threadAt(second);

    KT_EQU(t, b->sigPending, 0ull);
    KT_EQU(t, b->sigBlocked, 0ull);
    KT_EQU(t, b->sigHandlerDepth, 0u);
    KT_EQU(t, b->sigSavedBlocked[0], 0ull);
    for (int signumIndex = 1; signumIndex < sigMaxSignals; signumIndex++) {
        KT_EQU(t, b->sigHandlers[signumIndex], sigHandlerDefault);
        KT_EQU(t, b->sigHandlerMasks[signumIndex], 0ull);
    }

    KT_TRUE(t, threadDiscard(second));
}

KTEST(sigConsumePendingPopulatesSigInfoForSigFlagSigInfoHandler)
{
    uint32 id = threadCreate("sigInfoTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    sigSetHandler(thread, sigUsr1, 0x40009000ull, 0, sigFlagSigInfo);
    KT_TRUE(t, sigRaiseInfo(id, sigUsr1, siCodeUser, 4242, 99));

    Registers regs = makeUserRegs();
    int signum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_EQ(t, signum, sigUsr1);

    KT_EQU(t, regs.rdx, 0ull);

    KT_NE(t, regs.rsi, 0ull);
    SigInfo* info = (SigInfo*)regs.rsi;
    KT_EQ(t, info->siSignum, sigUsr1);
    KT_EQ(t, info->siCode, siCodeUser);
    KT_EQ(t, info->siPid, 4242);
    KT_EQ(t, info->siStatus, 99);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigConsumePendingLeavesRsiRdxUntouchedWithoutSigFlagSigInfo)
{
    uint32 id = threadCreate("sigNoInfoTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    sigSetHandler(thread, sigUsr1, 0x40009000ull, 0, 0);
    KT_TRUE(t, sigRaise(id, sigUsr1));

    Registers regs = makeUserRegs();
    regs.rsi = 0x5151515151515151ull;
    regs.rdx = 0x6262626262626262ull;
    int signum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);

    KT_EQU(t, regs.rsi, 0x5151515151515151ull);
    KT_EQU(t, regs.rdx, 0x6262626262626262ull);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigRaisePlainWrapperDefaultsToSiCodeKernelWithNoSenderOrStatus)
{
    uint32 id = threadCreate("sigInfoDefaultTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    sigSetHandler(thread, sigHup, 0x40009000ull, 0, sigFlagSigInfo);
    KT_TRUE(t, sigRaise(id, sigHup));

    Registers regs = makeUserRegs();
    int signum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);

    SigInfo* info = (SigInfo*)regs.rsi;
    KT_EQ(t, info->siSignum, sigHup);
    KT_EQ(t, info->siCode, siCodeKernel);
    KT_EQ(t, info->siPid, 0);
    KT_EQ(t, info->siStatus, 0);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigRaiseInfoOverwritesStalePendingInfoOnReRaiseBeforeDelivery)
{
    uint32 id = threadCreate("sigInfoOverwriteTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);

    sigSetHandler(thread, sigUsr2, 0x40009000ull, 0, sigFlagSigInfo);
    KT_TRUE(t, sigRaiseInfo(id, sigUsr2, siCodeUser, 111, 1));
    KT_TRUE(t, sigRaiseInfo(id, sigUsr2, siCodeUser, 222, 2));

    Registers regs = makeUserRegs();
    int signum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);

    SigInfo* info = (SigInfo*)regs.rsi;
    KT_EQ(t, info->siPid, 222);
    KT_EQ(t, info->siStatus, 2);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigRaiseGroupDeliversToEveryLiveMemberOfThatGroupOnly)
{
    uint32 a = threadCreate("sigGroupA", noopThreadEntry);
    uint32 b = threadCreate("sigGroupB", noopThreadEntry);
    uint32 outsider = threadCreate("sigGroupOutsider", noopThreadEntry);
    KT_NE(t, a, threadInvalidId);
    KT_NE(t, b, threadInvalidId);
    KT_NE(t, outsider, threadInvalidId);

    uint32 pgid = threadAt(a)->pgid;
    KT_TRUE(t, threadSetPgid(b, pgid));
    KT_NE(t, threadAt(outsider)->pgid, pgid);

    KT_TRUE(t, sigRaiseGroup(pgid, sigUsr1, siCodeKernel, 0));
    KT_EQU(t, threadAt(a)->sigPending & (1ull << sigUsr1), (1ull << sigUsr1));
    KT_EQU(t, threadAt(b)->sigPending & (1ull << sigUsr1), (1ull << sigUsr1));
    KT_EQU(t, threadAt(outsider)->sigPending, 0ull);

    KT_TRUE(t, threadDiscard(a));
    KT_TRUE(t, threadDiscard(b));
    KT_TRUE(t, threadDiscard(outsider));
}

KTEST(sigRaiseGroupReturnsFalseWhenNoLiveThreadHasThatPgid)
{
    KT_FALSE(t, sigRaiseGroup(0xBEEF0001u, sigUsr1, siCodeKernel, 0));
}

KTEST(sigRaiseGroupSkipsDeadOrUnusedMembersButStillFindsLiveOnes)
{
    uint32 a = threadCreate("sigGroupDeadA", noopThreadEntry);
    uint32 b = threadCreate("sigGroupDeadB", noopThreadEntry);
    KT_NE(t, a, threadInvalidId);
    KT_NE(t, b, threadInvalidId);

    uint32 pgid = threadAt(a)->pgid;
    KT_TRUE(t, threadSetPgid(b, pgid));
    KT_TRUE(t, threadKill(b));

    KT_TRUE(t, sigRaiseGroup(pgid, sigUsr2, siCodeKernel, 0));
    KT_EQU(t, threadAt(a)->sigPending & (1ull << sigUsr2), (1ull << sigUsr2));
    KT_EQU(t, threadAt(b)->sigPending, 0ull);

    KT_TRUE(t, threadDiscard(a));
    KT_TRUE(t, threadReap(b));
}

KTEST(sigRaiseGroupWithSigStopStopsEveryLiveMemberOfThatGroup)
{
    uint32 a = threadCreate("sigGroupStopA", noopThreadEntry);
    uint32 b = threadCreate("sigGroupStopB", noopThreadEntry);
    KT_NE(t, a, threadInvalidId);
    KT_NE(t, b, threadInvalidId);

    uint32 pgid = threadAt(a)->pgid;
    KT_TRUE(t, threadSetPgid(b, pgid));

    KT_TRUE(t, sigRaiseGroup(pgid, sigStop, siCodeKernel, 0));
    KT_EQ(t, threadAt(a)->state, threadStopped);
    KT_EQ(t, threadAt(b)->state, threadStopped);

    KT_TRUE(t, threadContinue(a));
    KT_TRUE(t, threadContinue(b));
    KT_TRUE(t, threadDiscard(a));
    KT_TRUE(t, threadDiscard(b));
}
