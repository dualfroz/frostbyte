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

