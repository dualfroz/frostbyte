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

    KT_TRUE(t, threadReap(deadParent));
    KT_TRUE(t, threadReap(orphan));
}

/*
 * threadJoinAnyChild (sched.c) -- waitpid(-1)-style "wait for ANY child"
 * behind sysWait's waitAnyPid case (syscall.c) / userWaitAny (ulib.c). Live
 * proof that it genuinely blocks and is genuinely woken by an arbitrary
 * child's death (not just a specifically-named one) is user/waitanytest.c
 * (needle "waitanytest: any-child-reap-set-ok" in tools/ci-test.sh); what
 * is deterministic here is its own non-blocking scan logic, the same shape
 * as the parentId/sigChld block above.
 */

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
    /* Not reaped -- still exactly as threadCreate left it. */
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
    /* allocSlot (sched.c) always hands out the lowest currently-free slot
       and nothing else has been created in between, so childHigh's own id
       is guaranteed higher than childLow's -- the "lowest index wins"
       assertion below actually depends on this. */
    KT_LT(t, childLow, childHigh);
    threadAt(childLow)->parentId = parent;
    threadAt(childHigh)->parentId = parent;

    /* Kill the HIGHER-indexed one first, on purpose: proves the scan below
       picks the lowest matching slot, not whichever died most recently. */
    KT_TRUE(t, threadKill(childHigh));
    KT_TRUE(t, threadKill(childLow));

    uint32 outId = 0xFFFFFFFFu;
    int64 outCode = -1;
    KT_EQ(t, threadJoinAnyChild(parent, &outId, &outCode), waitScanFoundDead);
    KT_EQU(t, outId, childLow);
    KT_EQ(t, outCode, threadKilledExitCode);
    /* childLow is fully reaped already -- its slot is threadUnused now. */
    KT_EQ(t, threadAt(childLow)->state, threadUnused);
    /* childHigh is untouched by that call: still Dead, NOT yet reaped. */
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

    /* Fabricate exactly what sysWait's waitAnyPid path leaves behind via
       schedBlockCurrent(currentThread()) when parent itself is the one
       calling it (see threadStopExcludesTargetFromUnrelatedWaitChannelWake
       below for the same white-box convention) -- channel is the PARENT's
       own slot address, not the child's, unlike
       threadKillWakesAThreadParkedInJoin's channel above. */
    threadAt(parent)->state = threadBlocked;
    threadAt(parent)->waitChannel = threadAt(parent);

    KT_TRUE(t, threadKill(child));
    KT_EQ(t, threadAt(parent)->state, threadRunnable);

    KT_TRUE(t, threadReap(child));
    KT_TRUE(t, threadDiscard(parent));
}

/*
 * threadStop/threadContinue (sched.c) -- job control (signal.h's SIGSTOP/
 * SIGCONT/SIGTSTP) on top of the new threadStopped state. The live proof
 * that SIGSTOP genuinely stops a thread parked mid-syscall and SIGCONT
 * genuinely resumes it through the exact restart-syscall path sysRead
 * already uses is user/jobtest.c spawning user/jobtarget.c (needle
 * "jobtest: job-stop-continue-ok" in tools/ci-test.sh); what is
 * deterministic here is threadStop/threadContinue's own state-machine
 * bookkeeping, mirroring threadKillRejects.../threadKillMarks... above.
 */

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

    /* Idempotent: stopping an already-Stopped thread still succeeds,
       matching real kill(pid, SIGSTOP) sent a second time. */
    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    KT_TRUE(t, threadContinue(id));
    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadStopAcceptsSleepingBlockedAndRunningStates)
{
    uint32 id = threadCreate("stopFromEveryState", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    /* Fabricated states, the same way threadReapClosesOpenWritePipeFdSoReaderSeesEof
       below pokes threadDead directly: this thread is never actually
       scheduled in this pre-schedStart() harness, so its `state` field
       stands in for "genuinely Sleeping/Blocked/Running" -- exactly the
       states a real SIGSTOP target could be caught in. */
    threadAt(id)->state = threadSleeping;
    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);
    KT_TRUE(t, threadContinue(id));

    threadAt(id)->state = threadBlocked;
    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);
    KT_TRUE(t, threadContinue(id));

    /* Running -- the self-stop case (sysSigSend targeting the caller's own
       pid, syscall.c): threadStop does not special-case "am I the one
       calling this" the way threadKill implicitly does (Running is simply
       absent from threadKill's accepted set); a thread can stop itself. */
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

    /* threadUnused again after reap: still rejected. */
    KT_FALSE(t, threadStop(id));
}

KTEST(threadContinueRejectsOutOfRangeAndNonStoppedThreads)
{
    KT_FALSE(t, threadContinue(threadCapacity()));
    KT_FALSE(t, threadContinue(threadCapacity() + 1000));

    uint32 id = threadCreate("notStopped", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    /* Runnable, never stopped: SIGCONT against a process that was never
       stopped is a harmless no-op at this level (see signal.c's
       sigRaise, which still delivers the signal itself regardless). */
    KT_FALSE(t, threadContinue(id));
    KT_EQ(t, threadAt(id)->state, threadRunnable);

    KT_TRUE(t, threadDiscard(id));
}

KTEST(threadStopExcludesTargetFromUnrelatedWaitChannelWake)
{
    int channel;

    uint32 id = threadCreate("stoppedWhileWaiting", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    /* Fabricate exactly what schedBlockCurrent would have left behind had
       this thread been the one calling it (see
       threadDiscardClosesOpenReadPipeFdSoWriterSeesBrokenPipe below for
       the same fabricate-then-assert convention). */
    threadAt(id)->state = threadBlocked;
    threadAt(id)->waitChannel = &channel;

    KT_TRUE(t, threadStop(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);

    /* The defining difference from threadBlocked: schedWakeChannel must
       NOT match a Stopped thread even though its waitChannel field still
       names this exact channel -- only threadContinue (an explicit
       SIGCONT) may move it back to threadRunnable, never "the resource it
       was originally waiting on became available". */
    schedWakeChannel(&channel);
    KT_EQ(t, threadAt(id)->state, threadStopped);

    KT_TRUE(t, threadContinue(id));
    KT_EQ(t, threadAt(id)->state, threadRunnable);

    KT_TRUE(t, threadDiscard(id));
}

/*
 * threadStop/threadContinue's own new SIGCHLD-on-stop/SIGCHLD-on-continue
 * (sched.c, mirroring threadExit/threadKill's parentId/sigChld block
 * above): real job control notifies the parent asynchronously the instant
 * a child stops or resumes, not only when it eventually dies. The live
 * ring-3 proof that this reaches a genuinely blocked-mid-syscall target
 * and a real installed handler is user/stopchldtest.c spawning
 * user/stopchldtarget.c (needle "stopchldtest: job-control-sigchld-ok" in
 * tools/ci-test.sh); what is deterministic here is the same plumbing shape
 * threadKillRaisesSigChldAgainstParentId/
 * threadKillEarlyWakesAParentGenuinelyBlockedWaitingForSigChld above
 * already establish for the death case.
 */

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

    /* Simulate the parent's own handler already having consumed that one
       notification (sigConsumePending, signal.c, clears the bit on
       delivery) -- exactly the "next natural ring-3 return" a real
       sigchldtest.c-style handler would have already run through before a
       second, redundant sigStop against the same still-Stopped target
       could possibly arrive. */
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

    /* Same real-handler-required setup as
       threadKillEarlyWakesAParentGenuinelyBlockedWaitingForSigChld above --
       sigChld's default disposition is Ignore, so sigWouldDeliver would
       (correctly) decline to early-wake without one. */
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
    /* Drain the stop's own notification first (see the "already stopped"
       test above for why) so the assertion below is unambiguously about
       threadContinue's OWN raise, not a leftover bit from the stop. */
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

    /* Same sigRaise targetId-state guard as
       threadKillAgainstAnOrphanWithADeadParentIdRaisesNothing above -- an
       orphan whose spawning parent already died has nowhere for its own
       stop notification to land either. */
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

    /* Already Runnable/Running: nothing to force. */
    KT_EQ(t, threadAt(id)->state, threadRunnable);
    KT_FALSE(t, threadWakeForSignal(id));
    KT_EQ(t, threadAt(id)->state, threadRunnable);

    threadAt(id)->state = threadRunning;
    KT_FALSE(t, threadWakeForSignal(id));
    KT_EQ(t, threadAt(id)->state, threadRunning);

    /* Stopped is the one deliberately EXCLUDED state -- an ordinary signal
       must never undo job control's SIGSTOP; only threadContinue (an
       explicit SIGCONT) may ever move a Stopped thread. This is what
       signal.c's sigRaise relies on for a signal raised against a Stopped
       target: sigWouldDeliver may say yes, but threadWakeForSignal still
       refuses. */
    KT_TRUE(t, threadStop(id));
    KT_FALSE(t, threadWakeForSignal(id));
    KT_EQ(t, threadAt(id)->state, threadStopped);
    KT_TRUE(t, threadContinue(id));

    threadAt(id)->state = threadDead;
    KT_FALSE(t, threadWakeForSignal(id));
    KT_TRUE(t, threadReap(id));

    /* threadUnused again after reap: still rejected. */
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

/*
 * threadReap/threadDiscard's own fd cleanup (sched.c's reclaimOpenFds) -- a
 * thread that dies while still holding an open fdPipeRead/fdPipeWrite fd
 * must give pipe.c's readers/writers count back exactly like an orderly
 * vfsClose would, or the pipe's OTHER end (a second, independent process,
 * e.g. fifomaster.c's children) would wait forever for an EOF/broken-pipe
 * that can now never come -- the same shape of leak FALA 13's bug#2 found
 * for console readers, just against pipe.c's own refcounts. These fabricate
 * the fd the same way the threadCreateUser tests above fabricate a user
 * slot: poke threadAt(id)->fds[] directly (bypassing vfsPipeAttach's own
 * allocFd/currentThread(), since the thread under test is never actually
 * running) with a real pipeCreateDetached handle, so the underlying pipe.c
 * refcounts driving the assertions are the genuine article, not a mock.
 */

KTEST(threadDiscardClosesOpenReadPipeFdSoWriterSeesBrokenPipe)
{
    int handle = pipeCreateDetached();
    KT_GE(t, handle, 0);

    uint32 id = threadCreate("pipeReadHolder", noopThreadEntry);
    KT_NE(t, id, threadInvalidId);

    /* Simulate this (never-run) thread having opened the read end via
       vfsPipeAttach: a real reader attached to the handle, referenced only
       from this thread's own fd table (index 3: 0-2 are always fdConsole,
       see vfsFdInit). */
    pipeAddReader(handle);
    threadAt(id)->fds[3].kind = fdPipeRead;
    threadAt(id)->fds[3].pipeHandle = handle;

    /* A second, independent side attaches the write end -- tracked purely
       by handle, with no fd of its own, exactly like pipeShareTest.c's
       coverage of the same primitives. */
    pipeAddWriter(handle);
    KT_FALSE(t, pipeWriteBroken(handle));

    /* Runnable, never scheduled: threadDiscard's path (e.g. rolling back a
       thread that never got to run its own vfsClose). */
    KT_TRUE(t, threadDiscard(id));

    /* The only reader was this thread's fd -- once reclaimOpenFds has run,
       the writer must see the pipe as genuinely, permanently broken, not
       "try again" forever. */
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
    /* Ring empty, a writer is still attached: "try again", not EOF yet. */
    KT_EQ(t, pipeRead(handle, buffer, sizeof(buffer)), -1);

    /* No live scheduler to run threadExit through before schedStart() --
       poke the state directly, the same workaround
       threadReapOnDeadUserThreadReclaimsItsMappedPages uses above for the
       same reason. */
    threadAt(id)->state = threadDead;
    KT_TRUE(t, threadReap(id));

    /* The only writer was this thread's fd -- once reclaimOpenFds has run,
       the reader must see real end-of-file (0), not "try again" (-1)
       forever. */
    KT_EQ(t, pipeRead(handle, buffer, sizeof(buffer)), 0);

    pipeCloseRead(handle);
}

/* threadSetAlarm (sched.c) always targets currentThread() -- there is no
   target-id argument, exactly like real alarm(2) -- so these tests exercise
   it exactly the way syscall.c's sysAlarm does, against whatever thread is
   "current" in this pre-schedStart() harness (the permanent idle thread,
   id 0), rather than a freshly created one the way most other tests here
   fabricate their own target. That is safe: deliverPendingSignals (isr.c)
   is a no-op for anything that is not a ring-3 return context (`(regs->cs &
   3) != 3`), so a pending sigAlrm bit left on the idle thread by an
   in-progress deadline is never actually consumed, and schedule()'s own
   alarm-expiry scan (see its file comment) clears alarmAt back to 0 the
   first tick it notices the deadline has passed either way -- but each test
   below still explicitly cancels (threadSetAlarm(0)) before returning, so
   no stray deadline is left for a LATER test or real boot to trip over. */
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

    /* Replace a 10s alarm with a 3s one: the return value describes the
       OLD (10s) deadline, not the new one, and the struct itself now holds
       the new one -- matching real alarm()'s "at most one pending alarm,
       setting a new one silently discards whatever was left of the old"
       contract. No real time passes between these two calls (interrupts
       are off, there is no scheduler to preempt this function), so the
       ceiling in threadSetAlarm's own comment reports the full, exact 10,
       not 9 or 11 -- a flaky "off by the current tick's remainder" result
       here would itself be a bug in that rounding, not a timing accident. */
    KT_EQU(t, threadSetAlarm(10), 0ull);
    KT_EQU(t, threadSetAlarm(3), 10ull);

    /* Canceling (0) reports the 3s alarm's remaining time and clears the
       deadline back to "none pending" -- the other real alarm(0) case,
       exercised separately from the plain replace case above. */
    KT_EQU(t, threadSetAlarm(0), 3ull);
    KT_EQU(t, self->alarmAt, 0ull);

    /* Canceling an ALREADY-canceled (none pending) alarm reports 0, the
       same "nothing was pending" value a fresh thread starts with. */
    KT_EQU(t, threadSetAlarm(0), 0ull);
}

/*
 * Thread.pgid (sched.h) -- process groups, the scope real job control and
 * kill(2)/sigSend's pid<0 convention actually target (signal.c's
 * sigRaiseGroup, sched.c's threadKillGroup, syscall.c's sysKill/sysSigSend,
 * keyboard.c's real Ctrl-C/Ctrl-Z). ktestRunAll always runs as the
 * permanent idle thread (id 0) -- exactly the one context initialPgid
 * (sched.c) treats as "boot-time", per this file's own header comment --
 * so every plain threadCreate/threadCreateUser call here exercises the
 * "becomes its own new group leader" default; the OTHER default (a
 * runtime child inheriting a genuinely running, non-idle parent's pgid)
 * can only ever be exercised by a real ring-3 userSpawn, which is why
 * user/jobtest.c's own pgid check (spawning user/jobtarget.c at runtime)
 * is this feature's live proof instead.
 */

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

/*
 * Thread.uid (sched.h) -- the credential vfs.c's vfsPermitted consults
 * alongside a file's own owner uid and mode (see coreTest.c's own
 * vfsPermitted coverage for that pure-logic half). Same "ktestRunAll
 * always runs as the idle thread" reasoning as the pgid tests immediately
 * above applies here too: every plain threadCreate/threadCreateUser call
 * exercises the "boot-time thread starts as uid 0" default; the OTHER
 * default (a runtime child inheriting a genuinely running, non-root
 * parent's uid) can only ever be exercised by a real ring-3 userSpawn,
 * which is why user/identtest.c's own inheritance-then-drop demo is this
 * feature's live proof instead. currentThreadSetUid's own privilege rule
 * (root may become anyone; anyone else only their own current uid) is
 * likewise never exercised here on purpose: it can only ever act on
 * "whoever is currently running" (see its own doc comment), and mutating
 * ktestRunAll's own idle-thread uid away from 0 would permanently corrupt
 * every later test AND every boot-time spawnUser call still to come after
 * ktestRunAll returns -- a real, one-way privilege drop is exactly the
 * kind of state no shared test-suite context may ever touch.
 */

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

    /* Each started as its own group leader (the KTEST-runs-as-idle default
       above) -- explicitly move joiner into leader's group, exactly like a
       real setpgid(joinerPid, leaderPid) call. */
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
    /* A completely different group must be untouched -- the exact scoping
       property keyboard.c's real Ctrl-C/Ctrl-Z depends on (user/pgroupfg.c/
       user/pgroupbg.c is the live ring-3 proof for sigRaiseGroup's own
       version of this same scoping, signal.c). */
    KT_EQ(t, threadAt(outsider)->state, threadRunnable);

    KT_TRUE(t, threadReap(a));
    KT_TRUE(t, threadReap(b));
    KT_TRUE(t, threadDiscard(outsider));
}

KTEST(threadKillGroupReturnsFalseWhenNoLiveThreadHasThatPgid)
{
    /* No thread created anywhere in this whole KTEST run ever uses this
       exact value as its pgid -- ids never reach anywhere near it (see
       threadCapacity/maxThreads), so it can never collide with a real
       group by coincidence. */
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

    /* b is killed FIRST, independently -- threadKillGroup must still find
       and kill a (already-Dead members are simply skipped, exactly like
       sigRaiseGroup below skips them via sigRaiseInfo's own Dead/Unused
       guard), and must still report true since a real target existed. */
    KT_TRUE(t, threadKill(b));
    KT_TRUE(t, threadKillGroup(pgid));
    KT_EQ(t, threadAt(a)->state, threadDead);

    KT_TRUE(t, threadReap(a));
    KT_TRUE(t, threadReap(b));
}

/* currentThreadGetEnv/currentThreadSetEnv (sched.h's new Thread.env) always
   act on whoever is "current" -- at KTEST time that is the idle thread
   (id 0, same "always-present current thread" reasoning this file's own
   header comment already gives for schedBlockCurrent above), so every test
   below resets threadAt(0)->envCount to 0 first: idle's own table is pure
   scratch space for these tests (initialEnv gives every REAL boot-time
   spawn an empty table regardless of whatever idle's own happens to hold,
   see initialEnv's own "currentIndex == 0" branch, sched.c), but leaving
   it dirty between tests would let an earlier test's entries leak into a
   later one's count/index assumptions. */
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
    /* An overwrite must never consume a second slot. */
    KT_EQ(t, currentThreadEnvCount(), 1);
}

KTEST(currentThreadSetEnvRejectsEntryTooLongForStorage)
{
    threadAt(0)->envCount = 0;

    /* envEntryMax (sched.h) bounds "NAME=VALUE\0" together -- a value long
       enough to blow that budget must be rejected outright, leaving no
       partially-written entry behind. */
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

    /* Overwriting an EXISTING name must still work even while the table is
       completely full -- it takes the already-occupied slot, never a new
       one. */
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

/* currentThreadUnsetEnv (sched.h) -- real unsetenv(3): a no-op success on a
   missing name, a real compacting removal on a present one (never just a
   tombstone, since currentThreadEnvEntry's own index space must stay dense
   for shell.c's `env` listing to keep working after a removal in the
   middle). */
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

    /* Removing the MIDDLE of three entries must shift every LATER entry
       down by one slot (memmove, not just marking a hole) -- otherwise
       currentThreadEnvEntry(1) after this removal would either still show
       the removed "B=2" or skip straight to garbage past envCount. */
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

    /* Fill the table completely, remove one, then prove a genuinely NEW
       name can now be set -- if unset only tombstoned the slot instead of
       compacting envCount back down, this set would still see a "full"
       table and fail. */
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
