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

/*
 * ktestRunAll() runs before schedStart() (see ktest.h), on a single thread
 * with no live scheduler - there is no second execution context to actually
 * context-switch into, and no interrupts to wake a genuinely halted CPU.
 * The first block of tests below therefore exercises schedBlockCurrent/
 * schedWakeChannel as the pure state-machine they are (mark-blocked /
 * match-and-wake), using the always-present current thread (idle, id 0 at
 * this point in boot) rather than creating new threads.
 *
 * The second block tests threadCreate/threadReap/threadJoin's bookkeeping
 * (slot allocation, exhaustion, reuse, bounds-checking) directly: a thread
 * created here is never actually scheduled to run (the scheduler is not yet
 * live), so as long as every thread a test creates is reaped again before
 * the test returns, the real thread table is left exactly as it found it -
 * no throwaway slot is ever left behind to eat into kmain.c's later budget.
 * The one path that genuinely cannot be tested here is threadJoin() ever
 * actually parking (schedBlockCurrent + halt()) on a thread that is still
 * alive: that requires a second thread for the scheduler to switch to,
 * which does not exist before schedStart(). That path is instead proven
 * live by the join demo in kmain.c (needle "join-reap-ok" in ci-test.sh).
 */

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

    /* Same address every call -- every not-yet-satisfied sysPoll (poll.h)
       across every thread blocks on this ONE shared channel, so callers
       must never see it move. */
    KT_EQP(t, schedPollChannel(), schedPollChannel());
    KT_NOTNULL(t, schedPollChannel());

    /* And it works exactly like any other channel schedBlockCurrent/
       schedWakeChannel already accept -- the schedule()-driven pollDeadline
       expiry scan (sched.c) that forces a wake even with nobody calling
       schedWakeChannel(schedPollChannel()) needs a live scheduler
       (schedulerActive) to run at all, so it is proven live instead, by
       user/polltest.c's real timeout phase (tools/ci-test.sh's
       "polltest: poll-timeout-ok" needle) -- the same split schedTest.c's
       own file comment already documents for sleepUntil/alarmAt. */
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

    /* Nobody is blocked on this address: waking it must not touch any
       thread's state, including the caller's own. */
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

    /* Waking the stale first channel must not affect a thread that has since
       re-blocked on a different one. */
    schedWakeChannel(&channelA);
    KT_EQ(t, currentThread()->state, threadBlocked);

    schedWakeChannel(&channelB);
    KT_EQ(t, currentThread()->state, threadRunnable);

    currentThread()->state = saved;
}

/* Never actually invoked (the scheduler is not live yet) -- just a valid
   function pointer for threadCreate's entry parameter. If it were ever
   scheduled for real it would be a harmless, correctly-behaved kernel
   thread body (park forever), same convention as workerA/workerB. */
static void noopThreadEntry(void)
{
    for (;;)
        halt();
}

KTEST(threadCreateExhaustsThenThreadDiscardFreesSlotsBackForReuse)
{
    /* Sized off threadCapacity() itself, not a hardcoded number that would
       silently stop meaning "the whole table" the next time maxThreads
       (sched.c) grows -- exactly the staleness this test's own array size
       once had (fixed at 64, which only ever happened to equal maxThreads
       by coincidence of when it was written; the very next maxThreads bump
       after this one otherwise made the while loop below stop at the array
       bound before threadCreate ever actually failed, so the "must refuse
       to grow past capacity" check two lines down silently stopped testing
       anything and leaked one never-discarded thread for the rest of
       boot). idle already occupies one slot, so successful creates always
       stay strictly below `capacity`, and the loop below is guaranteed to
       hit its threadInvalidId break at or before `count == capacity - 1` --
       this array can never actually overflow. */
    uint32 capacity = threadCapacity();
    uint32 created[capacity];
    uint32 count = 0;

    /* Every thread created here stays Runnable/never-run for its entire
       life in this test (schedule() never runs before schedStart()), so
       threadDiscard -- not threadReap, which requires threadDead -- is the
       correct way to tear them back down. */
    while (count < capacity) {
        uint32 id = threadCreate("ktestDummy", noopThreadEntry);
        if (id == threadInvalidId)
            break;
        created[count++] = id;
    }

    /* The table must have refused to grow past its fixed capacity instead
       of silently writing past threads[] -- this is the bounds-check the
       original allocator did not have. */
    KT_EQU(t, threadCreate("shouldFail", noopThreadEntry), threadInvalidId);

    for (uint32 i = 0; i < count; i++)
        KT_TRUE(t, threadDiscard(created[i]));

    /* Every discarded slot must be usable again: the very next create
       should succeed (it would fail immediately if discard had left the
       table full). */
    uint32 reused = threadCreate("afterDiscard", noopThreadEntry);
    KT_NE(t, reused, threadInvalidId);
    KT_TRUE(t, threadDiscard(reused));
}

KTEST(threadCreateSkipsAFreshlyFreedLowSlotInFavorOfMovingForward)
{
    /* allocSlot (sched.c) used to always hand out the LOWEST currently-free
       slot -- this test used to assert exactly that (KT_EQ(reused, b)
       after discarding the middle one of three). That was a real,
       reproducible bug: this fala's own gate caught killtest.c's and
       nicetest.c's "a pid I just userWait()-reaped must reject a further
       kill/nice" checks occasionally failing for real, because some
       completely unrelated thread elsewhere in the system called
       threadCreate in the tiny window between the reap and the recheck
       and got handed that exact just-freed low index straight back. The
       fix -- allocSlot's own new round-robin cursor -- means the very
       next create after freeing a LOW slot, with plenty of headroom still
       available, deliberately does NOT reuse it: it keeps moving forward
       and grows the high-water mark instead, exactly like a real OS's own
       pid allocator avoids immediately handing back a pid that just
       exited. See allocSlot's own doc comment for the full story and
       why this is a practical mitigation via headroom, not a mathematical
       impossibility. */
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

    /* b itself is still sitting there Unused, unreachable until the
       cursor eventually wraps back around to it -- the very next test
       below proves it is not leaked forever, just deferred. */
    KT_EQU(t, threadPgid(b), threadInvalidId);
}

KTEST(threadCreateReusesTheOnlyFreedSlotOnceTheTableIsFull)
{
    /* Same "must reuse freed slots, not just grow the high-water mark
       forever" property the old lowest-index-specific test above used to
       cover, restated so it holds regardless of WHERE allocSlot's cursor
       happens to be: fill the ENTIRE table (sized off threadCapacity()
       itself, same staleness-proofing reasoning as
       threadCreateExhaustsThenThreadDiscardFreesSlotsBackForReuse above),
       free exactly one slot picked from the middle, then prove the very
       next create can ONLY land on that one slot -- there is nowhere else
       left to land, so the round-robin scan is forced all the way around
       to it regardless of its own starting position. */
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
    /* id 0 is the permanent idle thread: everRan is true for it from
       schedInit onward, so discard must refuse it. */
    KT_FALSE(t, threadDiscard(0));

    /* Past the fixed capacity entirely. */
    KT_FALSE(t, threadDiscard(threadCapacity()));
    KT_FALSE(t, threadDiscard(threadCapacity() + 1000));

    uint32 id = threadCreate("discardMe", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);
    KT_TRUE(t, threadDiscard(id));

    /* Already threadUnused: a second discard on the same id must fail
       rather than double-free the (already-freed) stack. */
    KT_FALSE(t, threadDiscard(id));
}

KTEST(threadReapRejectsIdleOutOfRangeAndNeverCreatedSlots)
{
    /* id 0 is the permanent idle thread: it is always Running/Runnable,
       never Dead, so reap must refuse it. */
    KT_FALSE(t, threadReap(0));

    /* Past the fixed capacity entirely. */
    KT_FALSE(t, threadReap(threadCapacity()));
    KT_FALSE(t, threadReap(threadCapacity() + 1000));

    /* A slot nobody has ever created a thread in yet is threadUnused, not
       threadDead. A freshly created, never-run thread (Runnable) is not
       threadDead either -- reap must refuse it too, distinctly from
       threadDiscard above. */
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
    /* All of these must return immediately (no schedBlockCurrent/halt())
       since there is no live scheduler to ever wake this thread back up --
       a wrong implementation here would hang the whole test battery, not
       just fail an assertion. */
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

    /* The slot is threadUnused again -- joining it must behave exactly like
       joining an id that was never created, not crash or block. */
    KT_EQ(t, threadJoin(id), -1);
}

/*
 * threadCreateUser/threadReap/threadDiscard user-memory reclamation
 * (Thread.userSlotBase, sched.h). Every real spawn/exec used to leak its
 * whole elfSlotSize slot's worth of physical frames on death, since
 * frostbyte's single shared page table is never touched by thread teardown
 * -- these tests drive that path directly with a synthetic "process" (its
 * own fake slot, far above any real USER_BASE any binary is linked at, so
 * it can never collide with a real program's mapping) rather than relying
 * only on the live boot demo (kmain.c's proctest/spawntarget/exectarget
 * chain, needles "user-mem-reclaimed-ok"/"exec-old-slot-freed-ok" in
 * ci-test.sh) to prove it.
 */

KTEST(threadCreateUserSetsUserSlotBaseFromEntry)
{
    uint64 entry = 0x320000000ull + 0x2000;
    uint32 id = threadCreateUser("probe", entry, elfSlotStackTop(entry));
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadUserSlotBase(id), elfSlotBase(entry));
    KT_EQU(t, threadUserSlotBase(id), 0x320000000ull);
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadReapOnDeadUserThreadReclaimsItsMappedPages)
{
    uint64 fakeSlotBase = 0x330000000ull;
    uint64 entry = fakeSlotBase + 0x2000;
    uint64 stackTop = elfSlotStackTop(entry);
    uint64 stackSize = 4 * frameSize;
    uint64 before = pmmFreeFrames();

    /* Mirror what a real loadImageByPath/spawnUser call maps for any user
       thread: a sliver of "image" plus an eagerly-mapped stack, both well
       inside the 256 MiB slot -- most of the slot stays unmapped, exactly
       like a real small program. */
    /* At least the image page plus the stack pages -- entry and fakeSlotBase
       are virgin address space, so mapping also pays for whatever PDPT/PD/PT
       page-table pages do not exist yet (paging.c's nextTable create path),
       which this test does not try to predict the exact count of. The reap/
       discard side below is the precise assertion: every frame spent here,
       data or structural, must come all the way back. */
    KT_TRUE(t, pagingMapUserRange(entry, frameSize));
    KT_TRUE(t, pagingMapUserRange(stackTop - stackSize, stackSize));
    KT_LE(t, pmmFreeFrames(), before - 1 - (stackSize / frameSize));

    uint32 id = threadCreateUser("fakeProc", entry, stackTop);
    KT_NE(t, id, threadInvalidId);
    KT_EQU(t, threadUserSlotBase(id), fakeSlotBase);

    /* No live scheduler to run threadExit through before schedStart() --
       poke the state directly, the same workaround
       schedBlockCurrentMarksBlockedWithChannel uses above for the same
       reason. */
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

    /* At least the image page plus the stack pages -- entry and fakeSlotBase
       are virgin address space, so mapping also pays for whatever PDPT/PD/PT
       page-table pages do not exist yet (paging.c's nextTable create path),
       which this test does not try to predict the exact count of. The reap/
       discard side below is the precise assertion: every frame spent here,
       data or structural, must come all the way back. */
    KT_TRUE(t, pagingMapUserRange(entry, frameSize));
    KT_TRUE(t, pagingMapUserRange(stackTop - stackSize, stackSize));
    KT_LE(t, pmmFreeFrames(), before - 1 - (stackSize / frameSize));

    uint32 id = threadCreateUser("fakeProc2", entry, stackTop);
    KT_NE(t, id, threadInvalidId);

    /* Runnable, never scheduled: the threadDiscard path, not threadReap. */
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

/*
 * threadKill (sched.c) -- the only way to recover a thread genuinely stuck
 * forever, now that sysRead/sysWrite can really block on a shared pipe
 * (pipe.h/vfs.h). The live proof that it tears down a thread ACTUALLY
 * parked mid-syscall is killtest.c spawning killvictim.c (needles
 * "killtest: killed victim"/"killtest: kill-ok" in tools/ci-test.sh); what
 * is deterministic here is threadKill's own state-machine bookkeeping.
 */

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

    /* A killed thread reaps exactly like one that called threadExit on its
       own -- no separate "killed" bookkeeping survives past exitCode. */
    KT_TRUE(t, threadReap(id));
}

KTEST(threadKillOnAlreadyDeadOrUnusedSlotFails)
{
    uint32 id = threadCreate("killTwice", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, threadKill(id));
    /* Already Dead: a second kill must not succeed (nor reset exitCode). */
    KT_FALSE(t, threadKill(id));
    KT_EQU(t, threadAt(id)->exitCode, threadKilledExitCode);

    KT_TRUE(t, threadReap(id));
    /* threadUnused again after reap: still rejected. */
    KT_FALSE(t, threadKill(id));
}

KTEST(threadKillWakesAThreadParkedInJoin)
{
    ThreadState saved = currentThread()->state;

    uint32 victim = threadCreate("victim", noopThreadEntry);
    KT_NE(t, victim, threadInvalidId);

    /* Mirrors exactly what threadJoin (sched.c) itself does while waiting
       for `victim`: block the current thread (the idle thread, id 0, at
       this point in boot) on the victim slot's own address -- the same
       channel threadExit/threadKill both wake. */
    schedBlockCurrent(threadAt(victim));
    KT_EQ(t, currentThread()->state, threadBlocked);

    KT_TRUE(t, threadKill(victim));
    KT_EQ(t, currentThread()->state, threadRunnable);

    currentThread()->state = saved;
    KT_TRUE(t, threadReap(victim));
}

/*
 * Thread.parentId (sched.h) and the real, asynchronous SIGCHLD it drives
 * from threadExit/threadKill (signal.h/signal.c's sigRaise) -- the live
 * ring-3 proof that a genuinely blocked parent actually wakes for its own
 * child's death is user/sigchldtest.c spawning user/sigchldchild.c (needle
 * "sigchldtest: sigchld-delivered-to-parent-ok" in tools/ci-test.sh); what
 * is deterministic here is the plumbing itself: threadCreate/
 * threadCreateUser attributing parentId to whoever is truly running at
 * creation time, and threadKill (parameterized by an explicit id, unlike
 * threadExit which only ever acts on the CURRENT thread and so cannot be
 * driven this way with ktestRunAll's single, never-context-switched
 * thread -- see this file's own header comment) actually raising sigChld
 * against exactly that id when the id it names dies.
 */

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

    /* ktestRunAll runs single-threaded (see file header) -- threadCreate
       always attributes parentId to THIS KTEST runner itself, so the
       "child was spawned by parent" relationship this test actually wants
       is set up directly here, the same white-box technique
       threadStopExcludesTargetFromUnrelatedWaitChannelWake below already
       uses to reach a state the real call path would only reach through
       an actual live sysSpawn. */
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

    /* sigChld's real-world default disposition is Ignore (sigDefaultIsIgnore,
       signal.c), which sigWouldDeliver (also signal.c) treats exactly like
       an installed sigHandlerIgnore -- correctly declining to early-wake a
       thread for a signal that would just be silently dismissed on
       delivery anyway, real Unix's own "SIGCHLD does nothing without a
       handler" behavior. A parent that actually wants this notification
       (like user/sigchldtest.c) always installs a real handler first, so
       this test must too, or threadWakeForSignal below would (correctly)
       never fire and this would be testing the wrong thing. */
    sigSetHandler(threadAt(parent), sigChld, 0x40005000ull, 0, 0);

    /* Fabricate exactly what schedBlockCurrent would have left behind had
       parent been the one calling it (see
       threadStopExcludesTargetFromUnrelatedWaitChannelWake above for the
       same convention) -- e.g. a real sigchldtest.c genuinely blocked in
       userRead on a pipe with no writer (see user/sigchldtest.c) while
       its own sigChld handler is installed. threadWakeForSignal's own
       state-machine bookkeeping is already proven generically by
       threadWakeForSignalMovesSleepingOrBlockedThreadToRunnable above (for
       any signal); this proves specifically that threadKill's own new
       sigRaise(parentId, sigChld) call actually reaches that same
       early-wake path for THIS signal, not just the pending-bit half. */
    threadAt(parent)->state = threadBlocked;
    threadAt(parent)->waitChannel = &channel;

    KT_TRUE(t, threadKill(child));
    KT_EQ(t, threadAt(parent)->state, threadRunnable);

    KT_TRUE(t, threadReap(child));
    /* Runnable and never actually scheduled (ktestRunAll runs
       single-threaded, see file header) -- threadDiscard, not threadReap,
       is the correct teardown here, same as every other fabricated-but-
       never-run thread in this file. */
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

    /* sigRaise's own targetId-state guard (signal.c) is what makes this
       safe, not anything special here -- an orphan whose spawning parent
       is already gone simply has nowhere for sigChld to land, the same
       way a real kernel would reparent it to init instead of crashing or
       misdelivering into whatever unrelated thread later reuses
       deadParent's slot; this kernel does not implement reparenting (one
       shared address space, no true fork(), see doSpawn's own file
       comment in syscall.c), so the signal is just silently dropped. */
    KT_TRUE(t, threadKill(orphan));
    KT_EQ(t, threadAt(orphan)->state, threadDead);
    KT_EQU(t, threadAt(deadParent)->sigPending & (1ull << sigChld), 0ull);

