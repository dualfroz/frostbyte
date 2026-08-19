#include "ktest.h"
#include "sched.h"
#include "signal.h"
#include "ports.h"
#include "string.h"
#include "gdt.h"

/*
 * Pure logic for signal.c's delivery decision (sigConsumePending) and its
 * supporting state (sigRaise/sigSetHandler/sigProcMask/sigResetState/
 * sigOnSigReturn) -- all fully testable without a live scheduler or a real
 * ring-3 transition, since they only ever touch a Thread's own fields and a
 * caller-supplied Registers snapshot. The one thing this file cannot cover
 * (a real trapframe actually being rewritten so iretq lands ring 3
 * execution inside the shared trampoline page, the handler genuinely
 * running, and sysSigReturn genuinely resuming afterwards) is proven live
 * instead by sigtest.c/sigtarget.c/sigdefault.c (ci-test.sh needles
 * "sigtest: handler-roundtrip-ok" / "sigtest: default-terminate-ok").
 */

static void noopThreadEntry(void)
{
    for (;;)
        halt();
}

/* A plausible "interrupted in ring 3, mid-instruction" snapshot: distinct,
   recognizable values in every field sigConsumePending/sigOnSigReturn are
   expected to touch (rip/rax/rdi) or must leave alone until sigOnSigReturn
   restores them (rbx/rsp/cs/ss/rflags), so a test can tell "changed" from
   "still original" without ambiguity. */
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

    /* Raising the same signal again while it is still pending changes
       nothing -- there is exactly one bit per signal number, not a queue
       or a count, matching real Unix's non-queuing signals. */
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

    /* Slot just discarded is threadUnused again -- a signal aimed at it
       must fail exactly like one aimed at a never-created id. */
    KT_FALSE(t, sigRaise(id, sigUsr1));
}

KTEST(sigRaiseSigKillDelegatesToThreadKill)
{
    uint32 id = threadCreate("sigKillTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    /* threadKill only accepts Runnable/Sleeping/Blocked; a freshly created
       thread is Runnable, so this is the same live-target case
       threadKillMarksRunnableThreadDeadWithKilledExitCode already covers
       for threadKill directly -- this test is about sigRaise's OWN
       dispatch reaching it, not re-testing threadKill's state machine. */
    KT_TRUE(t, sigRaise(id, sigKill));
    KT_EQ(t, threadAt(id)->state, threadDead);
    KT_EQU(t, threadAt(id)->exitCode, threadKilledExitCode);

    /* sigKill never touches sigPending -- it bypasses the pending/blocked
       machinery entirely (see signal.h's file comment). */
    KT_EQU(t, threadAt(id)->sigPending, 0ull);

    KT_TRUE(t, threadReap(id));
}

KTEST(sigRaiseSigStopDelegatesToThreadStopAndBypassesPending)
{
    uint32 id = threadCreate("sigStopTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, sigRaise(id, sigStop));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    /* Like sigKill, sigStop never touches sigPending -- it bypasses the
       pending/blocked/handler machinery entirely (see signal.h's file
       comment and sigRaiseSigKillDelegatesToThreadKill above). */
    KT_EQU(t, threadAt(id)->sigPending, 0ull);

    /* Real kill(pid, SIGKILL) kills a stopped process too (threadKill was
       widened to accept threadStopped alongside threadStop's own
       introduction). */
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

    /* sigCont, unlike sigStop, still falls through to the generic pending
       path afterwards -- but the resume itself (threadContinue) happens
       first, as an unconditional side effect, regardless of sigBlocked:
       blocking EVERYTHING (short of sigKill/sigStop, which sigProcMask
       always strips) must not stop the actual resume. */
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

    /* Never stopped: SIGCONT here is a no-op at the process-state level
       (real Unix: continuing an already-running process does nothing extra),
       but the signal itself is still delivered -- an installed handler
       still runs through the normal sigConsumePending scan regardless. */
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

    /* No handler installed: sigTerm's default disposition is Terminate,
       which sigWouldDeliver treats as genuinely deliverable -- exactly the
       user/eintrtarget.c scenario (a thread blocked on a condition that
       will never naturally occur, recovered by an ORDINARY signal, not
       threadKill's direct bypass). */
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

    /* sigUsr1 explicitly blocked: must not disturb the sleep at all, same
       as real Unix -- a blocked signal cannot interrupt anything. */
    sigProcMask(thread, sigMaskBlock, (1ull << sigUsr1));
    thread->state = threadSleeping;
    KT_TRUE(t, sigRaise(id, sigUsr1));
    KT_EQ(t, thread->state, threadSleeping);
    KT_TRUE(t, thread->sigPending & (1ull << sigUsr1));

    /* sigChld's real-world default is Ignore -- dismissed outright, never
       delivered, so it must not wake anything either. */
    thread->state = threadBlocked;
    KT_TRUE(t, sigRaise(id, sigChld));
    KT_EQ(t, thread->state, threadBlocked);

    /* An explicit sigHandlerIgnore on an otherwise-Terminate signal is the
       same story. */
    sigSetHandler(thread, sigUsr2, sigHandlerIgnore, 0, 0);
    KT_TRUE(t, sigRaise(id, sigUsr2));
    KT_EQ(t, thread->state, threadBlocked);

    threadAt(id)->state = threadRunnable;
    KT_TRUE(t, threadDiscard(id));
}

KTEST(sigRaiseDoesNotEarlyWakeForSignalMaskedByActiveHandlersSaMask)
{
    /* The gap the two tests above do NOT cover: sigWouldDeliver (and
       therefore threadWakeForSignal) reads thread->sigBlocked verbatim,
       with no idea whether any given bit got there via an explicit
       sigProcMask call (tested above) or via sigConsumePending's own
       handler-entry union of sigHandlerMasks[signum] (the file comment's
       per-signal sa_mask paragraph). This drives the SECOND path for real
       -- actually entering a handler via sigConsumePending, not just
       calling sigProcMask by hand -- to prove the early-wake mechanism
       (FALA 16) genuinely respects a mask that arrived that way too, the
       exact interaction FALA 19 flagged as unexamined before deferring
       this whole topic. user/masktarget.c is this same scenario live: its
       onUsr1 handler (sa_mask = sigHup's bit here, sigUsr2's there) blocks
       on a real read WHILE the mask is active, so an early wake bug here
       would hang it forever instead of just failing a check. */
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

    /* onUsr1 is now "running" (as far as this thread's own state is
       concerned) and immediately blocks on something real, exactly like
       user/masktarget.c's own userRead(goFd) call from inside its
       handler. */
    thread->state = threadBlocked;

    /* sigHup is covered by onUsr1's sa_mask -- sigWouldDeliver must say no,
       so threadWakeForSignal is never even called: the Blocked state (and
       therefore whatever real wait this models) must survive completely
       undisturbed, same as the explicit-sigProcMask case above. */
    KT_TRUE(t, sigRaise(id, sigHup));
    KT_EQ(t, thread->state, threadBlocked);
    KT_TRUE(t, thread->sigPending & (1ull << sigHup));

    /* Once onUsr1 genuinely returns (sigOnSigReturn, restoring sigBlocked
       to what it was before -- no sigHup bit), the deferred sigHup is
       deliverable again, but this thread is STILL Blocked (the read it was
       modelling has not itself completed) -- sigOnSigReturn only unmasks,
       it is not itself a wake source, so nothing here should move the
       thread back to Runnable behind the scheduler's own back. */
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

    /* sigTerm would genuinely deliver against a Runnable/Sleeping/Blocked
       target (sigWouldDeliver says yes -- no handler, default Terminate,
       not blocked) but job control's "stopped means stopped" invariant
       must survive it existing at all: only threadContinue (an explicit
       SIGCONT) may ever move a Stopped thread, exactly like real Unix
       (a stopped process does not die of SIGTERM either, until continued
       or SIGKILLed). */
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

    /* Installing a handler for a DIFFERENT signum must not disturb sigUsr1's
       own stored mask -- sigHandlerMasks[] is indexed by the signal being
       handled, one independent slot per signal number, same as
       sigHandlers[] itself already is. */
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

    /* sigKill can never be blocked, matching real Unix: its bit is
       silently stripped from whatever mask is passed in, for BOTH
       sigMaskBlock and sigMaskSetMask. */
    KT_EQU(t, sigProcMask(thread, sigMaskSetMask, sigAllBlocked), (1ull << sigTerm));
    KT_FALSE(t, thread->sigBlocked & (1ull << sigKill));
    /* sigStop can never be blocked either, same treatment as sigKill. */
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
    /* Blocked, not delivered: the pending bit must survive untouched for
       whenever it eventually gets unblocked. */
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

    /* sigChld's real-world default disposition is Ignore -- with no
       handler installed at all it must be silently dismissed, not
       terminate the thread. */
    sigRaise(id, sigChld);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeNone);
    KT_FALSE(t, thread->sigPending & (1ull << sigChld));

    /* An explicit sigHandlerIgnore dismisses a signal whose default would
       otherwise be Terminate. */
    sigSetHandler(thread, sigUsr1, sigHandlerIgnore, 0, 0);
    sigRaise(id, sigUsr1);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeNone);
    KT_FALSE(t, thread->sigPending & (1ull << sigUsr1));

    /* An ignored signal must not mask a REAL one raised alongside it in the
       same scan: sigHup (explicit sigHandlerIgnore here) sits at a LOWER
       signal number than sigTerm (default Terminate, no handler), so the
       scan hits the dismissed one first and must keep going instead of
       stopping there. */
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

    /* Terminate is reported, not carried out, here -- sigConsumePending has
       no way to hand back a new rsp for a thread it just killed, so it
       must not touch the trapframe at all; isr.c's deliverPendingSignals
       is the one that actually calls threadExit. */
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

    /* SIGTSTP's real-world default disposition is Stop, not Terminate --
       the catchable half of job control (see sigDefaultIsStop, signal.c).
       sigStop itself never reaches here at all (sigRaise bypasses this
       whole scan for it, see sigRaiseSigStopDelegatesToThreadStopAndBypassesPending
       above), so this is the ONLY way sigOutcomeStop is ever produced. */
    sigRaise(id, sigTstp);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeStop);
    KT_EQ(t, signum, sigTstp);
    KT_FALSE(t, thread->sigPending & (1ull << sigTstp));

    /* Reported, not carried out, here -- isr.c's deliverPendingSignals is
       the one that actually calls threadStop, the same contract
       sigOutcomeTerminate already has with threadExit. */
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

    /* Unlike SIGSTOP, SIGTSTP is fully catchable: an installed handler
       overrides the default Stop disposition, exactly like any other
       catchable signal overriding a default Terminate. */
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

    /* sigUsr1's sa_mask explicitly includes sigHup -- unlike the nesting
       test below (mask 0), THIS is what should keep sigHup from
       interrupting sigUsr1's handler at all. */
    uint64 usr1Handler = 0x40009000ull;
    uint64 hupHandler = 0x4000C000ull;
    sigSetHandler(thread, sigUsr1, usr1Handler, (1ull << sigHup), 0);
    sigSetHandler(thread, sigHup, hupHandler, 0, 0);

    sigRaise(id, sigUsr1);
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_EQ(t, signum, sigUsr1);
    KT_TRUE(t, thread->sigBlocked & (1ull << sigHup));

    /* sigHup arrives while sigUsr1's handler is active -- masked by its
       sa_mask, so it must stay pending, NOT nest, unlike an unmasked
       signal would (see the nesting test below). */
    sigRaise(id, sigHup);
    int deferredSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &deferredSignum), sigOutcomeNone);
    KT_EQ(t, deferredSignum, -1);
    KT_TRUE(t, thread->sigPending & (1ull << sigHup));
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    /* Once sigUsr1's handler returns, its sa_mask's grip on sigHup ends
       with it (sigBlocked restored to what it was before) -- the deferred
       signal is finally deliverable, as its own TOP-LEVEL handler, not a
       nested one. */
    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_FALSE(t, thread->sigBlocked & (1ull << sigHup));

    int finalSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &finalSignum), sigOutcomeHandler);
    KT_EQ(t, finalSignum, sigHup);
    KT_EQU(t, regs.rax, hupHandler);
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    KT_TRUE(t, threadDiscard(id));
}

/* Default (sigFlagNoDefer NOT set) half of the contrast: a signal raised
   again against ITSELF while its own handler is still active must stay
   merely pending, not nest -- sigConsumePendingCustomHandlerRewritesTrapframeAndPushesContext
   above already asserts sigUsr1's own bit lands in sigBlocked once its
   handler starts; this test is the direct behavioral consequence of that
   bit actually being there, driven the same way
   sigConsumePendingSaMaskDefersDifferentSignalUntilHandlerReturns drives
   sa_mask's deferral. */
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

    /* Same signum, raised again while its own handler is still active --
       self-blocked (no sigFlagNoDefer), so this must stay pending, not
       enter a second, nested instance of the same handler. */
    sigRaise(id, sigUsr1);
    int deferredSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &deferredSignum), sigOutcomeNone);
    KT_EQ(t, deferredSignum, -1);
    KT_TRUE(t, thread->sigPending & (1ull << sigUsr1));
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    /* Once the (only) active handler returns, its self-block lifts and the
       deferred re-raise is finally deliverable -- as a fresh TOP-LEVEL
       invocation (depth back to 1, not 2), never having nested at all. */
    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_FALSE(t, thread->sigBlocked & (1ull << sigUsr1));

    int finalSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &finalSignum), sigOutcomeHandler);
    KT_EQ(t, finalSignum, sigUsr1);
    KT_EQU(t, thread->sigHandlerDepth, 1u);

    KT_TRUE(t, threadDiscard(id));
}

/* sigFlagNoDefer's whole point: registered for a signal, that signal's own
   bit is no longer added to sigBlocked when its handler starts, so raising
   the SAME signum again while that handler is still active genuinely
   nests a second instance of it -- the direct opposite outcome of the
   test just above, same signum, only the flag differs. */
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
    /* The one thing sigFlagNoDefer actually changes: sigUsr1's own bit is
       NOT in sigBlocked, unlike the default case above. */
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
    /* Still no self-block even at the nested depth -- a third raise would
       nest again the same way, bounded only by sigMaxNestDepth, never by
       sigUsr1 blocking itself. */
    KT_FALSE(t, thread->sigBlocked & (1ull << sigUsr1));
    KT_EQ(t, memcmp(&thread->sigSavedRegs[1], &midHandler, sizeof(midHandler)), 0);

    /* Unwinding pops the nested instance first, resuming the outer one
       exactly where the re-raise interrupted it -- LIFO, same as any other
       nested delivery. */
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

    /* Trapframe rewritten to enter the trampoline: rip is the shared
       trampoline entry, rax is the handler address its `call *%rax`
       invokes, rdi is the signal number (the handler's sole argument). */
    KT_EQU(t, regs.rip, sigTrampolineVirtualAddr);
    KT_EQU(t, regs.rax, handlerAddr);
    KT_EQU(t, regs.rdi, (uint64)sigUsr1);
    /* Untouched: the trampoline runs on the interrupted thread's own
       existing stack, no separate signal stack. */
    KT_EQU(t, regs.rsp, before.rsp);

    /* sigUsr1's own sa_mask is empty (0 above), so the ONLY new bit added
       to sigBlocked is sigUsr1 itself (self-blocked, like real Unix) --
       whatever was already blocked (sigUsr2, from sigProcMask) survives
       untouched, a union not a replacement. */
    KT_EQU(t, thread->sigHandlerDepth, 1u);
    KT_EQU(t, thread->sigBlocked, blockedBefore | (1ull << sigUsr1));
    KT_EQU(t, thread->sigSavedBlocked[0], blockedBefore);

    /* The ENTIRE pre-signal trapframe must be recoverable later -- not
       just the fields this test happens to check above. */
    KT_EQ(t, memcmp(&thread->sigSavedRegs[0], &before, sizeof(before)), 0);

    /* An ALREADY-blocked signal (sigUsr2, blocked since before sigUsr1 was
       even raised) stays deferred while a handler is active, exactly like
       it would with no handler running at all -- the depth stack does not
       change what sigBlocked itself gates. */
    sigRaise(id, sigUsr2);
    int blockedSignum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &blockedSignum), sigOutcomeNone);
    KT_EQ(t, blockedSignum, -1);
    KT_TRUE(t, thread->sigPending & (1ull << sigUsr2));

    /* A signal NOT covered by sigUsr1's mask nor its own self-block --
       sigHup -- genuinely interrupts the active handler with a NESTED one
       of its own: this is the behavior a single "one handler at a time"
       bool (this code used to have) could never produce, since it gated
       delivery before sigBlocked was even consulted. */
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
    /* sigHup's own handler has no mask either, so its blocked set is
       whatever sigUsr1's handler already made it, plus sigHup's own bit. */
    KT_EQU(t, thread->sigBlocked,
           blockedBefore | (1ull << sigUsr1) | (1ull << sigHup));
    /* What got pushed for THIS (nested) frame is exactly the trapframe
       sigUsr1's handler was actually running with the instant sigHup
       preempted it -- not the original pre-sigUsr1 `before` snapshot. */
    KT_EQ(t, memcmp(&thread->sigSavedRegs[1], &midUsr1Handler,
                     sizeof(midUsr1Handler)), 0);
    KT_EQU(t, thread->sigSavedBlocked[1], blockedBefore | (1ull << sigUsr1));

    /* Unwinding: first sigOnSigReturn resumes sigUsr1's still-active
       handler exactly where sigHup interrupted it, not the original
       program -- LIFO, like a real call stack of signal frames. */
    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_EQ(t, memcmp(&regs, &midUsr1Handler, sizeof(midUsr1Handler)), 0);
    KT_EQU(t, thread->sigHandlerDepth, 1u);
    KT_EQU(t, thread->sigBlocked, blockedBefore | (1ull << sigUsr1));

    /* Second sigOnSigReturn finally resumes the ORIGINAL interrupted
       program, restoring the exact pre-sigUsr1 trapframe and blocked
       mask. */
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

    /* Poke the depth field directly to the bound rather than actually
       nesting sigMaxNestDepth real handlers -- this test is about the
       GUARD, which the fault-injection check below confirms is load-
       bearing (removing it does not, by itself, corrupt anything a plain
       "one more handler" scenario would exercise; it needs the table
       genuinely full to matter). */
    thread->sigHandlerDepth = sigMaxNestDepth;
    sigSetHandler(thread, sigUsr1, 0x40009000ull, 0, 0);
    sigRaise(id, sigUsr1);

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeNone);
    KT_EQ(t, signum, -1);
    /* Left pending, not lost -- exactly like a blocked signal, it simply
       waits for room (a sigOnSigReturn dropping the depth back down). */
    KT_TRUE(t, thread->sigPending & (1ull << sigUsr1));
    KT_EQU(t, thread->sigHandlerDepth, (uint32)sigMaxNestDepth);

    thread->sigHandlerDepth = 0;
    KT_TRUE(t, threadDiscard(id));
}

/*
 * SA_RESTART / EINTR (signalnum.h's sigFlagRestart, sched.h's
 * Thread.sigInRetryableSyscall) -- signal.h's file comment has the full
 * story. These four drive sigConsumePending directly instead of a live
 * syscall retry, the same division of labor the rest of this file already
 * uses: user/restarttarget.c/restarttest.c prove the real ring-3 round
 * trip, these prove the exact rip/rax rewrite (or lack of it) is gated on
 * precisely the right condition.
 */

KTEST(sigConsumePendingReturnsEintrForInterruptedRetryableSyscallWithoutRestartFlag)
{
    uint32 id = threadCreate("sigEintrTarget", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    Thread* thread = threadAt(id);
    Registers regs = makeUserRegs();
    /* Stand-in for the exact state sysRead/sysWrite/sysWait (syscall.c)
       leave a thread in right before blocking: rip rewound 2 bytes behind
       the `int $0x80` that trapped in, so re-entering ring 3 normally
       would re-execute it. */
    regs.rip = 0x40001232ull;
    int signum = -1;

    uint64 handlerAddr = 0x4000A000ull;
    sigSetHandler(thread, sigUsr1, handlerAddr, 0, 0);
    thread->sigInRetryableSyscall = true;
    sigRaise(id, sigUsr1);

    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);
    KT_EQ(t, signum, sigUsr1);

    /* The handler still runs normally (trampoline entry) -- the rewrite
       only touches the SAVED, about-to-resume context below, never how
       this handler itself gets entered. */
    KT_EQU(t, regs.rip, sigTrampolineVirtualAddr);
    KT_EQU(t, regs.rax, handlerAddr);

    /* What actually resumes once this handler returns: rip past the
       interrupted `int $0x80` entirely (not just past the rewind -- the
       retry itself is skipped), rax the raw negative "errno" a real
       syscall would return -- genuine EINTR, not a silent retry. */
    KT_EQU(t, thread->sigSavedRegs[0].rip, 0x40001232ull + 2);
    KT_EQU(t, thread->sigSavedRegs[0].rax, (uint64)(int64)errInterrupted);

    /* Consumed -- a later, nested signal must never see this as still
       true (see the next test). */
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

    /* sigFlagRestart: the saved, about-to-resume context is untouched --
       exactly the original retry point, so the syscall genuinely retries
       once this handler returns, same as every handler behaved before this
       flag existed. */
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

    /* No sigFlagRestart, AND this thread was never actually mid a
       retryable syscall (a fresh thread's sigInRetryableSyscall starts
       false) -- ordinary running code being interrupted, exactly like a
       plain sysSleep or no syscall at all. The gate must refuse to invent
       an EINTR here just because the flag is absent. */
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

    /* sigHup has no sigFlagRestart, but it never interrupted a retryable
       syscall -- it interrupted sigUsr1's OWN already-running handler.
       thread->sigInRetryableSyscall was already consumed (cleared) by the
       outer delivery above, so this nested push must leave regs alone
       regardless of sigHup's own flags: the saved context is exactly the
       mid-outer-handler snapshot, not an invented EINTR. */
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

    /* regs now holds whatever the trampoline/handler would have left it
       as by the time it traps into sysSigReturn -- simulate that instead
       of actually running user code, since this file has no ring 3. */
    regs.rax = 0xDEADBEEFull;
    regs.rdi = 0xFEEDFACEull;

    KT_TRUE(t, sigOnSigReturn(thread, &regs));
    KT_EQ(t, memcmp(&regs, &before, sizeof(before)), 0);
    KT_EQU(t, thread->sigHandlerDepth, 0u);
    KT_EQU(t, thread->sigBlocked, blockedBefore);

    /* Misuse: calling sigreturn while NOT actually inside a handler must be
       a safe no-op, not a crash or a silent corruption of an unrelated
       trapframe. */
    Registers untouched = regs;
    KT_FALSE(t, sigOnSigReturn(thread, &regs));
    KT_EQ(t, memcmp(&regs, &untouched, sizeof(untouched)), 0);

    KT_TRUE(t, threadDiscard(id));
}

/* A table slot recycled by threadDiscard/threadReap must never let a dead
   process's signal state leak into whatever unrelated process reuses its
   id next. Today that is guaranteed twice over -- threadReap/threadDiscard
   already memset() the whole Thread struct before any slot is handed back
   to allocSlot, and sigResetState does it again explicitly from
   threadCreate (see sched.h's field comment) -- so this test does not
   currently isolate sigResetState's own contribution; it locks in the
   observable end-to-end property regardless of which of the two is doing
   the work, the same way the console-reader/pipe-fd leak fixes (see
   keyboard.c/sched.c history) earned tests for state that lived OUTSIDE
   Thread and had no such blanket memset to fall back on. */
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
    /* Drives `a` into sigHandlerDepth==1 (via the installed handler above)
       so that field's reset gets exercised too, not just
       sigPending/sigBlocked/sigHandlers/sigHandlerMasks. */
    KT_EQ(t, sigConsumePending(a, &regs, &signum), sigOutcomeHandler);
    KT_EQU(t, a->sigHandlerDepth, 1u);

    KT_TRUE(t, threadDiscard(first));

    /* allocSlot's own round-robin cursor (sched.c) no longer guarantees
       the very next create reuses THIS exact slot -- drive it around
       until it does, discarding every other slot it hands back along the
       way, so this test still deterministically exercises a real recycle
       of `first` regardless of where the cursor currently sits. Bounded
       by threadCapacity(): the cursor sweeps every currently-live index
       at most once per full lap, and nothing else in this test holds a
       slot open forever, so `first` is guaranteed to come back around
       well within that many attempts. */
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

/*
 * sigFlagSigInfo / SigInfo (signalnum.h) -- signal.c's sigConsumePending
 * populating the shared sigInfoVirtualAddr page and rewriting rsi/rdx to
 * hand a sigFlagSigInfo handler a real (SigInfo*, NULL) pair, deterministic
 * counterpart to the live ring-3 round trip in siginfotest.c/
 * siginfotarget.c (ci-test.sh needles "siginfotarget: siginfo-sender-pid-ok"
 * / "siginfotest: sigchld-siginfo-accurate-ok"), which cannot itself prove
 * the EXACT bytes sigConsumePending computed the way direct access to
 * `regs`/the dereferenced pointer here can.
 */

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

    /* rdx is always NULL (no ucontext_t this kernel can expose) -- see
       signal.h's sigConsumePending doc comment. */
    KT_EQU(t, regs.rdx, 0ull);

    /* rsi is a real pointer into the shared, kernel-populated page -- not
       just "some nonzero value", dereferencing it must show exactly what
       sigRaiseInfo attached above. */
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

    /* No sigFlagSigInfo this time -- a plain one-argument handler, exactly
       like every signal demo before this session. */
    sigSetHandler(thread, sigUsr1, 0x40009000ull, 0, 0);
    KT_TRUE(t, sigRaise(id, sigUsr1));

    Registers regs = makeUserRegs();
    regs.rsi = 0x5151515151515151ull;
    regs.rdx = 0x6262626262626262ull;
    int signum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);

    /* Untouched, exactly as documented -- a handler with no
       sigFlagSigInfo never reads either register, so sigConsumePending
       must not spend any effort computing values for them. */
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
    /* The plain, pre-existing sigRaise -- every one of this file's other
       ~50 call sites uses exactly this, and none of them supply detail,
       so sigRaise's own thin-wrapper default (siCodeKernel/0/0) is what
       they all still get. */
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
    /* Same signum raised again before it was ever delivered -- non-queuing
       signals coalesce, so only the SECOND raise's detail should ever be
       observable, matching sigPending itself staying a single bit rather
       than a count. */
    KT_TRUE(t, sigRaiseInfo(id, sigUsr2, siCodeUser, 222, 2));

    Registers regs = makeUserRegs();
    int signum = -1;
    KT_EQ(t, sigConsumePending(thread, &regs, &signum), sigOutcomeHandler);

    SigInfo* info = (SigInfo*)regs.rsi;
    KT_EQ(t, info->siPid, 222);
    KT_EQ(t, info->siStatus, 2);

    KT_TRUE(t, threadDiscard(id));
}

/*
 * sigRaiseGroup (signal.h) -- the group-targeted half of real kill(2)'s
 * pid<0/pid==0 convention (syscall.c's sysSigSend) and keyboard.c's own
 * real Ctrl-C/Ctrl-Z, delivered to the FOREGROUND process group
 * (sched.h's Thread.pgid) rather than one hardcoded target. The live
 * ring-3 proof that this scoping actually protects a DIFFERENT group from
 * a keyboard-generated signal is user/pgroupfg.c (killed by it) versus
 * user/pgroupbg.c (proven still alive afterwards) in kmain.c's boot
 * sequence; what is deterministic here is the plain fan-out-and-filter
 * logic itself, mirroring schedTest.c's threadKillGroup coverage for the
 * exact same shape of primitive one layer down.
 */

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
    /* The whole point: a DIFFERENT group must be completely untouched --
       this is what keeps a real Ctrl-C from ever reaching a background
       job. */
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
    /* Dead members are silently skipped, exactly like sigRaiseInfo's own
       Dead/Unused guard already refuses a single-target send against one
       -- no crash, no stale sigPending bit written into a slot that is no
       longer this thread's own. */
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

    /* sigStop is delegated straight to threadStop per-target, bypassing
       sigPending entirely -- the same bypass sigRaiseInfo's own file
       comment documents for a single-target sigStop, now proven to apply
       independently to every member the group fan-out visits. */
    KT_TRUE(t, sigRaiseGroup(pgid, sigStop, siCodeKernel, 0));
    KT_EQ(t, threadAt(a)->state, threadStopped);
    KT_EQ(t, threadAt(b)->state, threadStopped);

    KT_TRUE(t, threadContinue(a));
    KT_TRUE(t, threadContinue(b));
    KT_TRUE(t, threadDiscard(a));
    KT_TRUE(t, threadDiscard(b));
}
