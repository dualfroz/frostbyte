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

