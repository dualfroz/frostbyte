#include "ktest.h"
#include "sched.h"
#include "pidfd.h"
#include "ports.h"

/*
 * Pure logic tests for pidfd.h/pidfd.c: no live scheduler needed (same
 * "runs before schedStart(), current thread is idle, every created thread
 * stays Runnable/never-run for its whole life" setup schedTest.c's own
 * threadCreate/threadDiscard tests already use), since every property here
 * is about sched.c's Thread.generation bookkeeping, not about actually
 * scheduling anything.
 *
 * The marquee test below (pidfdTargetDetectsSlotRecycledByAnUnrelatedThread)
 * is the deterministic, pure-logic reproduction of the exact scenario
 * killtest.c's own long-standing comment warns about but never guards
 * against ("killing it again must fail cleanly, not resurrect a slot that
 * has since been reused") and sched.c's own allocSlot doc comment
 * documents as a real, previously-unfixed race: a stale handle must never
 * silently act on whichever unrelated, live thread has since been handed
 * the exact same numeric slot. A live boot cannot reliably force this
 * exact collision on demand (allocSlot's round-robin cursor needs the
 * WHOLE table full before the very next create is forced back onto one
 * specific freed slot, see its own doc comment) -- exactly why this test
 * uses the same fill-the-table technique schedTest.c's own
 * threadCreateReusesTheOnlyFreedSlotOnceTheTableIsFull already established
 * for proving deterministic slot reuse, rather than a live demo (see
 * user/pidfdtest.c instead for the real ring-3 syscall-path proof of
 * post-reap staleness detection alone).
 */

static void noopThreadEntry(void)
{
    for (;;)
        halt();
}

KTEST(pidfdOpenRejectsIdleOutOfRangeAndUnusedTargets)
{
    KT_EQ(t, pidfdOpen(0), -1);
    KT_EQ(t, pidfdOpen(threadCapacity()), -1);
    KT_EQ(t, pidfdOpen(threadCapacity() + 1000), -1);

    /* Same "freed slot stays genuinely Unused until the round-robin cursor
       wraps back around to it" setup as schedTest.c's own
       threadCreateSkipsAFreshlyFreedLowSlotInFavorOfMovingForward. */
    uint32 a = threadCreate("pfA", noopThreadEntry);
    uint32 b = threadCreate("pfB", noopThreadEntry);
    uint32 c = threadCreate("pfC", noopThreadEntry);
    KT_TRUE(t, threadDiscard(b));

    KT_EQ(t, pidfdOpen(b), -1);

    KT_TRUE(t, threadDiscard(a));
    KT_TRUE(t, threadDiscard(c));
}

KTEST(pidfdOpenAndTargetRoundtripThenClose)
{
    uint32 x = threadCreate("pfX", noopThreadEntry);

    int handle = pidfdOpen(x);
    KT_GE(t, handle, 0);
    KT_EQU(t, pidfdTarget(handle), x);

    pidfdClose(handle);
    KT_EQU(t, pidfdTarget(handle), threadInvalidId);

    /* Closing twice must not corrupt anything else's slot. */
    pidfdClose(handle);
    KT_EQU(t, pidfdTarget(handle), threadInvalidId);

    KT_TRUE(t, threadDiscard(x));
}

KTEST(pidfdTargetDetectsDiscardedTarget)
{
    uint32 x = threadCreate("pfDiscarded", noopThreadEntry);
    int handle = pidfdOpen(x);
    KT_GE(t, handle, 0);

    KT_TRUE(t, threadDiscard(x));

    /* threadDiscard's memset reset Thread.generation for slot x back to 0
       -- pidfdTarget must notice immediately, with no new thread ever
       having claimed the slot yet. */
    KT_EQU(t, pidfdTarget(handle), threadInvalidId);

    pidfdClose(handle);
}

KTEST(pidfdTargetDetectsSlotRecycledByAnUnrelatedThread)
{
    uint32 capacity = threadCapacity();
    uint32 created[capacity];
    uint32 count = 0;

    while (count < capacity) {
        uint32 id = threadCreate("pfFill", noopThreadEntry);
        if (id == threadInvalidId)
            break;
        created[count++] = id;
    }
    KT_EQU(t, threadCreate("pfShouldFail", noopThreadEntry), threadInvalidId);

    uint32 victim = created[count / 2];
    int handle = pidfdOpen(victim);
    KT_GE(t, handle, 0);
    KT_EQU(t, pidfdTarget(handle), victim);

    /* victim is the ONLY free slot afterward -- the very next create is
       forced onto it regardless of allocSlot's own cursor position, the
       exact same proof schedTest.c's
       threadCreateReusesTheOnlyFreedSlotOnceTheTableIsFull already
       establishes for the underlying thread-table mechanics alone. */
    KT_TRUE(t, threadDiscard(victim));
    uint32 impostor = threadCreate("pfImpostor", noopThreadEntry);
    KT_EQ(t, impostor, victim);

    /* The old handle must NOT follow the slot to its new, completely
       unrelated occupant -- this is the entire point of pidfd.h over a
       raw cached pid, which would now (harmlessly, in this test, but not
       in general) apply any further kill/wait/sigSend straight to
       impostor while believing it was still acting on victim. */
    KT_EQU(t, pidfdTarget(handle), threadInvalidId);

    /* A FRESH pidfd opened on the same numeric id now correctly targets
       the new occupant -- the slot itself is not broken, only the OLD
       handle's own pinned incarnation is (rightly) stale. */
    int freshHandle = pidfdOpen(impostor);
    KT_GE(t, freshHandle, 0);
    KT_EQU(t, pidfdTarget(freshHandle), impostor);

    pidfdClose(handle);
    pidfdClose(freshHandle);
    for (uint32 i = 0; i < count; i++) {
        if (created[i] != victim)
            KT_TRUE(t, threadDiscard(created[i]));
    }
    KT_TRUE(t, threadDiscard(impostor));
}

KTEST(pidfdOpenExhaustsThenClosesFreeSlotsBackForReuse)
{
    uint32 x = threadCreate("pfTableFill", noopThreadEntry);

    int handles[maxPidfds];
    int count = 0;
    while (count < maxPidfds) {
        int h = pidfdOpen(x);
        if (h < 0)
            break;
        handles[count++] = h;
    }
    KT_EQ(t, count, maxPidfds);
    KT_EQ(t, pidfdOpen(x), -1);

    pidfdClose(handles[0]);
    int reused = pidfdOpen(x);
    KT_GE(t, reused, 0);
    KT_EQU(t, pidfdTarget(reused), x);

    pidfdClose(reused);
    for (int i = 1; i < count; i++)
        pidfdClose(handles[i]);
    KT_TRUE(t, threadDiscard(x));
}
