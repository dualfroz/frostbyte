#ifndef FROSTBYTE_SCHED_H
#define FROSTBYTE_SCHED_H

#include "types.h"
#include "vfs.h"
#include "prioq.h"
#include "isr.h"
#include "signalnum.h"
#include "mmap.h"

/* Default scheduling weight (xv6/CFS-style "nice" niceWeight -- higher runs
   more often, not longer). threadSetPriority scales relative to this. */
#define schedDefaultWeight 1024u

/* Returned by threadCreate/threadCreateUser when the thread table is full
   (see threadCapacity) -- never a valid thread id, since id 0 is always the
   permanent idle thread. Callers must check for this before using the
   result to index into anything thread-related. */
#define threadInvalidId 0xFFFFFFFFu

/* Capacity of Thread.name, including the terminator. Large enough for every
   name this kernel currently produces (kernel thread literals, "/bin/..."
   paths passed to threadCreateUser by spawnUser/doSpawn) with room to
   spare; longer names are truncated, never overflowed. */
#define threadNameMax 32

/* Capacity of Thread.env below: envMaxVars is how many "NAME=VALUE" slots
   a process may hold at once, envEntryMax is the max size in bytes of a
   single slot INCLUDING both the '=' separator and the nul terminator
   (so "NAME=VALUE\0" must fit in envEntryMax bytes). Generous enough for
   anything this kernel's own shell/demos actually set (see user/envprint.c)
   with room to spare, the same "small fixed table, not a growable heap
   allocation" choice Thread.cwd already made for a single path. */
#define envMaxVars 8
#define envEntryMax 64

/* One bit per signal number in Thread.sigPending/sigBlocked, and the size
   of Thread.sigHandlers[] below -- see signal.h for the actual signal
   subsystem this supports. Defined here, not signal.h, purely to break the
   include cycle (signal.h includes sched.h for the Thread type). */
#define sigMaxSignals 32

/* Depth of the per-thread nested-handler context stack (Thread.sigSavedRegs/
   sigSavedBlocked below) -- how many signal handlers may genuinely interrupt
   each other (handler A running, a second signal arrives and is not blocked
   by A's own sa_mask, so handler B runs nested inside A, then A resumes once
   B returns) before sigConsumePending (signal.c) refuses to deliver a new
   one and simply leaves it pending. A real kernel bounds this too (rlimit
   on the signal-frame stack it pushes into user memory); this one has no
   user-memory signal frame to overflow at all (see signal.h's file comment
   -- the saved context lives in the Thread struct itself), so the bound
   exists purely to keep that struct's size fixed and to fail a runaway
   signal-storm safely (further deliveries just wait) instead of unboundedly
   growing kernel memory per thread. 4 is generous headroom for anything
   this kernel's own handlers actually do (none nest more than one deep
   today -- see user/nesttarget.c) without being large enough to make
   Thread noticeably bigger than sigMaxSignals-worth of handlers already
   makes it. */
#define sigMaxNestDepth 4

typedef enum {
    threadUnused = 0,
    threadRunnable = 1,
    threadRunning = 2,
    threadSleeping = 3,
    threadDead = 4,
    threadBlocked = 5,
    /* Job-control stop (signal.h's SIGSTOP/SIGTSTP): excluded from
       schedule()'s ready-queue rebuild exactly like Sleeping/Blocked
       already are (only threadRunnable is ever inserted), so a stopped
       thread simply never gets picked until threadContinue moves it back
       to threadRunnable. Distinct from threadBlocked because it is NOT
       waiting on a wait-channel condition -- schedWakeChannel must never
       treat it as a match (see schedWakeChannel), only threadContinue
       (an explicit SIGCONT, not "the resource became available") ever
       moves a thread out of this state. */
    threadStopped = 6
} ThreadState;

typedef void (*ThreadEntry)(void);

typedef struct Thread {
    uint64 rsp;
    uint64 stackBase;
    uint64 kernelStackTop;
    uint64 sleepUntil;
    /* Absolute pitTicks() deadline for a pending alarm(2)-style SIGALRM
       (0 == no alarm pending), set by threadSetAlarm and consulted once
       per timer tick by schedule() -- the exact same shape as sleepUntil
       above, checked in the exact same loop, except expiry raises a real
       signal (sigRaise, signal.h) against the owning thread instead of
       just flipping its own ThreadState back to Runnable. Deliberately a
       plain deadline, not a wait-channel/ThreadState change: unlike
       threadSleep, a thread with a pending alarm keeps running (or
       sleeping, or blocked on something unrelated) exactly as it already
       was right up until the deadline, exactly like real alarm()'s timer
       ticking down in the background regardless of what the process is
       doing. */
    uint64 alarmAt;
    /* poll.h's sysPoll (syscall.c) uses the same restart-the-syscall park
       pattern sysRead/sysWrite/sysWait already use, but a poll call can be
       woken by any one of several unrelated fds, not one -- so instead of
       parking on a single fd's own wait channel (vfsBlockingChannel),
       sysPoll blocks every waiting thread on ONE shared channel
       (schedPollChannel below) and simply re-checks its whole fd list from
       scratch on every wake, spurious or not, exactly like the retry loop
       every OTHER restartable syscall already has. pollActive marks "this
       thread is mid a not-yet-satisfied sysPoll call" (so a retry entry
       can tell itself apart from a genuinely fresh call); pollDeadline is
       the absolute pitTicks() deadline schedule()'s own expiry scan forces
       a wake at if no fd ever becomes ready first (0 == wait forever,
       meaningful only while pollActive is true -- the same "0 reserved"
       convention alarmAt above already uses, but here 0 is a real,
       legal, requested state (a timeout of -1) rather than "nothing
       pending"). */
    bool pollActive;
    uint64 pollDeadline;
    uint64 brk;
    uint64 vruntime;
    uint64 exitCode;
    uint32 weight;
    uint32 id;
    /* Id of whichever thread was actually running (currentIndex, sched.c)
       at the moment this thread was created -- threadCreate/
       threadCreateUser both set this unconditionally, so it is always
       meaningful, not just for real userspace parent/child pairs: a
       boot-time spawnUser (kmain.c) call happens before schedStart() ever
       runs, so every process kmain.c spawns directly gets the permanent
       idle thread (id 0) as its parentId, which is deliberately never a
       valid sigRaise target (see signal.c's targetId==0 rejection) --
       only a RUNTIME sysSpawn (syscall.c's doSpawn, called from inside a
       live thread's own syscall) ever produces a parentId that is itself
       a live, signalable process. Consulted by threadExit/threadKill
       below to deliver a real SIGCHLD to whoever spawned this thread the
       instant it actually dies, matching real wait()/SIGCHLD semantics --
       NOT the same thing as "whoever later calls threadJoin/sysWait on
       this id", which can be anyone that knows the pid, not just the
       spawner (this kernel's honest fork()-less design already documents
       that distinction, see doSpawn's own file comment in syscall.c). */
    uint32 parentId;
    /* Distinguishes THIS specific occupant of slot `id` from any other
       thread that has occupied (or will ever occupy) the same numeric slot
       before or after -- the exact same "handle number alone is not enough
       once a slot is recycled" idea pipe.c's own Pipe.incarnation already
       solves for pipe handles (see pipeIncarnation's own doc comment,
       pipe.h). Assigned once, at creation time, from a single monotonic
       counter (sched.c's nextThreadGeneration, never reused, mirroring
       pipe.c's nextPipeIncarnation exactly) -- threadReap/threadDiscard's
       memset resets this back to 0 when the slot is freed, and the next
       threadCreate/threadCreateUser to land on that slot gets a brand new,
       never-before-seen value, so 0 here always means "this exact slot is
       currently unoccupied," never a live thread's own generation. Exists
       purely so pidfd.h's pidfdOpen/pidfdTarget can tell "the same thread I
       opened a handle on earlier" apart from "a different, unrelated thread
       that has since been handed the same slot number" -- allocSlot's own
       doc comment (sched.c) documents the real, previously-unguarded race
       this closes for any caller that opts into pidfd.h instead of a raw
       pid (kill/wait/sigSend/setpgid/nice below all still take a raw pid,
       completely unchanged, exactly as that same comment explains why). */
    uint32 generation;
    /* Process group id (POSIX pgid): the scope real job control and real
       kill(2)/sigSend's pid<0 convention actually target, not a single
       thread (signalnum.h's sigInt/sigTstp arriving from keyboard.c's real
       Ctrl-C/Ctrl-Z tracking are the live proof -- see keyboard.c's
       keyboardHandleScancode and signal.c's sigRaiseGroup). Set once, at
       creation time, by threadCreate/threadCreateUser's own initialPgid
       helper (sched.c): a thread created while the idle thread (id 0) is
       "current" -- i.e. one of kmain.c's boot-time spawnUser/threadCreate
       calls, the only time that can ever be true, see parentId's own
       comment just above -- becomes its own new group leader (pgid == its
       own id), exactly like a real init process placing each service it
       starts into its own job; a thread created by a genuinely running
       parent (any runtime sysSpawn, syscall.c's doSpawn) instead INHERITS
       that parent's pgid, matching real fork()'s own default (a child
       stays in its parent's process group until something explicitly
       calls setpgid). threadSetPgid (below) is the only thing that ever
       changes it again after creation -- the sysSetpgid syscall's
       userspace-facing half of real setpgid(2). */
    uint32 pgid;
    /* Real POSIX credential (uid(2)/setuid(2)): the identity vfs.c's
       vfsPermitted consults, alongside a file's own owner uid and mode, to
       decide owner-vs-other access at exactly the two enforcement choke
       points fs.h's fsModeOwner and fsModeOther bits already gate
       (resolveOpen's write check, loadImageByPath/jobctlSpawn's exec check) -- see
       vfsPermitted's own doc comment for the full owner/other algorithm
       (deliberately no uid-0-bypasses-everything superuser rule -- see that
       same comment for why). Same creation-time default/copy split as pgid
       immediately above (sched.c's new initialUid, mirroring initialPgid's
       own "is the idle thread currently running" test): a boot-time thread
       (kmain.c's spawnUser/threadCreate calls, idle currently "current")
       starts as uid 0 ("root" in name only here, not in VFS privilege --
       see fs.h's fsModeOwner* doc comment), exactly like a real init
       process; a genuine runtime child (syscall.c's doSpawn) instead
       INHERITS its parent's uid, matching real fork()'s own default before
       any setuid ever runs. currentThreadSetUid (below) is the only thing
       that ever changes it again after creation -- and, matching real
       setuid(2)'s own privilege rule, only ever lets uid 0 become anyone;
       any other uid may only "become" itself (a no-op), so dropping root is
       real and permanent for the lifetime of this thread, exactly like a
       real daemon that setuid()s away from root at startup and can never
       regain it. */
    uint32 uid;
    /* Real POSIX group credential (getgid(2)/setgid(2)): the group identity
       vfs.c's vfsPermitted consults, alongside a file's own group gid, as
       the MIDDLE fallback between the owner and other checks fs.h's
       fsModeGroup* bits gate. Same creation-time default/copy split as uid
       immediately above (sched.c's new initialGid, mirroring initialUid's
       own shape exactly): a boot-time thread starts at gid 0, a genuine
       runtime child (syscall.c's doSpawn) INHERITS its parent's gid.
       currentThreadSetGid (below) is the only thing that ever changes it
       again -- unlike currentThreadSetUid, the privilege gate for THIS
       call is the calling thread's own uid (currentThreadUid() == 0), not
       its gid, matching real setgid(2): a privileged (root) PROCESS may
       setgid to any group; an unprivileged one may only pass its own
       current gid back in (a no-op), so dropping to a group is real and
       permanent for the lifetime of a non-root thread, same shape as uid's
       own one-way drop. */
    uint32 gid;
    /* Real per-process working directory (POSIX chdir(2)/getcwd(2)): always
       a fully resolved absolute path (vfs.h's vfsChdir resolves a relative
       argument against the CALLING thread's own cwd before ever storing
       here, so this field itself never needs re-resolving against
       anything). Set once, at creation time, by threadCreate/
       threadCreateUser's own initialCwd helper (sched.c) -- a thread
       created while the idle thread (id 0) is "current" (kmain.c's
       boot-time spawns, see parentId's own comment above) starts at "/",
       exactly like a real init process; a thread created by a genuinely
       running parent (any runtime sysSpawn, syscall.c's doSpawn) instead
       INHERITS a byte-for-byte COPY of that parent's cwd, matching real
       fork()'s own copy-not-share semantics -- the two are independent
       buffers from that point on, so a child's later chdir() can never
       reach back and change its parent's, and vice versa. Before this
       field existed, the only cwd concept anywhere in this kernel was a
       single static local in shell.c shared by nothing else -- a spawned
       user process calling open()/mkdir()/etc with a relative path had
       never actually worked, since vfs.c's lookupNode/sfsLookup always
       resolved relative to the filesystem root, never to any notion of
       "current directory" (see vfs.c's vfsResolvePath, the only thing that
       reads this field). */
    char cwd[fsPathMax];
    /* Real per-process environment variables (POSIX getenv(3)/setenv(3)):
       up to envMaxVars "NAME=VALUE\0" strings, each at most envEntryMax-1
       bytes, plus envCount tracking how many slots are actually in use.
       Same creation-time default/copy split as Thread.cwd immediately
       above (sched.c's new initialEnv, mirroring initialCwd's own shape):
       a boot-time thread (idle is currently running) starts with an EMPTY
       table, like a real init process before anything ever calls
       setenv(); a genuine runtime child (syscall.c's doSpawn) instead
       inherits a byte-for-byte COPY of its parent's table, so a child's
       later setenv() can never reach back and change its parent's, and
       vice versa -- the exact same independent-buffer guarantee cwd
       already gives. */
    char env[envMaxVars][envEntryMax];
    int envCount;
    ThreadState state;
    /* Set once, permanently, the first time schedule() ever actually
       switches execution to this thread. Distinguishes "freshly created,
       never scheduled even once" (state threadRunnable, everRan false) from
       "was running and got preempted, still Runnable" (everRan true) -- the
       two look identical by state alone, but only the former is safe for
       threadDiscard to tear down (see threadDiscard). */
    bool everRan;
    /* Fixed buffer, not a pointer: threadCreateUser/doSpawn (syscall.c) may
       be called with a name that lives in a caller's stack-local buffer
       (a path just copied in from userspace), which does not outlive the
       call. thread->name must remain valid for the thread's entire life
       (shell.c's `ps` listing and the exit log both read it long after
       creation), so the bytes are always copied in, never aliased -- see
       threadSetName/copyThreadName in sched.c. */
    char name[threadNameMax];
    const void* waitChannel;
    /* Base virtual address of this thread's elfSlotSize-sized user-memory
       slot (image + stack + brk-heap all live within
       [userSlotBase, userSlotBase+elfSlotSize), see elf.h) -- 0 for kernel
       threads (threadCreate), which own no user pages to give back.
       Set from the entry point by threadCreateUser at spawn time and
       updated by exec() (syscall.c doExec) when a thread's image moves to
       a different slot. threadReap/threadDiscard pass this to
       pagingUnmapUserRange so a dead or rolled-back process's physical
       frames are actually reclaimed instead of leaking forever. */
    uint64 userSlotBase;
    PrioQNode schedNode;
    FileDescriptor fds[maxOpenFiles];
    /* mmap.h's own per-thread mapping table -- real demand-paged mmap(2)/
       munmap(2), resolved lazily by isr.c's #PF handler through
       mmapFaultHandler rather than eagerly mapped like pagingMapUserRange's
       image/stack/brk pages. Lives inside the same [userSlotBase,
       userSlotBase+elfSlotSize) slot as everything else this thread owns
       (see mmap.h's own mmapReserve doc comment for exactly which sub-range),
       so threadReap/threadDiscard's existing whole-slot
       pagingUnmapUserRange already reclaims every frame a mapping here ever
       faulted in with zero changes of their own -- this array only ever
       needs its OWN bookkeeping cleared (mmapReleaseAll), which the
       memset() both of those already do to the whole struct handles for
       free. */
    Vma vmas[maxVmasPerThread];
    /* Signal state (signal.h/signal.c). Explicitly zeroed by sigResetState
       from threadCreate/threadCreateUser below, the same way every other
       field above is explicitly reset there too -- threadReap/threadDiscard
       already memset() this whole struct before any slot is ever handed
       back to allocSlot, so this is belt-and-suspenders, not a fix for a
       leak that could otherwise happen (unlike keyboard.c's console-reader
       table or pipe.c's refcounts, which live OUTSIDE Thread and genuinely
       needed their own explicit reclaim -- see reclaimOpenFds). */
    uint64 sigPending;
    uint64 sigBlocked;
    uint64 sigHandlers[sigMaxSignals];
    /* Per-signal sa_mask (signal.h/signal.c): the ADDITIONAL signals to
       block, alongside the one being delivered, for as long as
       sigHandlers[signum]'s handler is running -- set by sigSetHandler,
       consulted only by sigConsumePending's custom-handler branch. Index is
       the signal being handled, not the signals it blocks (mirrors
       sigHandlers[] being indexed by signal number, not by handler). */
    uint64 sigHandlerMasks[sigMaxSignals];
    /* Per-signal sa_flags (signal.h/signal.c) -- today only sigFlagRestart
       (signalnum.h) is a defined bit, stored here exactly like
       sigHandlerMasks is: set by sigSetHandler, indexed by the signal being
       handled, consulted only by sigConsumePending when it is about to
       decide whether an interrupted retryable syscall (see
       sigInRetryableSyscall below) resumes transparently or returns
       errInterrupted. */
    uint64 sigHandlerFlags[sigMaxSignals];
    /* SigInfo (signalnum.h) attached to whichever instance of signum is
       CURRENTLY pending (index == signal number, same convention as
       sigHandlers/sigHandlerMasks/sigHandlerFlags above) -- set by
       sigRaiseInfo (signal.c) the moment sigPending's bit for it is set,
       read (and copied into the shared user-visible page) by
       sigConsumePending only if that signal's handler was registered with
       sigFlagSigInfo. Like sigPending itself, this does not queue: raising
       the same signum again before it is ever delivered overwrites this
       with the newer detail, matching real Unix's own non-queuing standard
       (non-realtime) signals, where only the most recent instance's info is
       ever actually observable once more than one arrives uncollected. */
    SigInfo sigPendingInfo[sigMaxSignals];
    /* True exactly while this thread is parked mid the restart-syscall
       pattern sysRead/sysWrite/sysWait/sysPoll (syscall.c) use to block: rip
       has already been rewound by the interrupted `int $0x80`'s 2 bytes so
       re-entering ring 3 re-executes the syscall from scratch. Set by
       syscall.c immediately before each of those four calls parks and
       reschedules; cleared unconditionally at the top of every
       syscallDispatch call (a fresh dispatch, whether this is the retry
       itself or a completely unrelated syscall, is never "still blocked" by
       definition) AND by sigConsumePending the moment it actually acts on a
       pending signal for this thread, win or lose -- a signal delivered
       while this is false (ordinary running code, a sleep, or a nested
       signal arriving inside an already-running handler) never touches
       rip/rax at all, exactly as if sigFlagRestart's whole mechanism did
       not exist, since there is no interrupted syscall retry to redirect. */
    bool sigInRetryableSyscall;
    /* How many handler contexts deep this thread currently is (0 = running
       ordinary code, not inside any handler) -- sigSavedRegs[0..depth-1]/
       sigSavedBlocked[0..depth-1] are the LIFO stack of interrupted contexts
       sigOnSigReturn pops from, one per still-active handler, exactly like a
       real call stack of signal frames. sigConsumePending refuses to push a
       new one once this reaches sigMaxNestDepth. */
    uint32 sigHandlerDepth;
    uint64 sigSavedBlocked[sigMaxNestDepth];
    Registers sigSavedRegs[sigMaxNestDepth];
} Thread;

void schedInit(void);

/* Allocates a slot from the fixed-size thread table (reusing the lowest
   free slot left behind by threadReap before ever growing the high-water
   mark) and initializes a new thread in it. Returns threadInvalidId,
   touching nothing else, if the table is completely full (threadCapacity
   threads already live) -- callers MUST check for this instead of assuming
   the id is always valid. */
uint32 threadCreate(const char* name, ThreadEntry entry);
uint32 threadCreateUser(const char* name, uint64 entry, uint64 userStackTop);
uint64 schedule(uint64 currentRsp);
void schedStart(void);

/* Total number of thread-table slots (used and free). A fixed compile-time
   constant, exposed so callers (and tests) can reason about exhaustion
   without hardcoding the internal capacity. */
uint32 threadCapacity(void);

/* Releases thread `id`'s resources (kernel stack) and resets its slot back
   to threadUnused so a later threadCreate/threadCreateUser can reuse it.
   Only valid on a thread whose state is threadDead (i.e. it has already
   called threadExit) -- reaping anything else (including the permanent
   idle thread, a still-live thread, or an id that was never created / was
   already reaped) is rejected and returns false with no effect. */
bool threadReap(uint32 id);

/* Tears down a thread that was created but has never actually been
   scheduled (state threadRunnable and everRan false) -- e.g. rolling back a
   spawn that failed a later setup step after threadCreate/threadCreateUser
   already succeeded. Unlike threadReap this does not require the thread to
   have exited: it requires the opposite, that it has not yet run even
   once, since anything that has run may hold state (a partially unwound
   stack, in-flight I/O) that is not safe to discard out from under it.
   Returns false, changing nothing, if the thread has ever run, is not
   currently Runnable (already sleeping/blocked/dead/unused), or id is out
   of range. */
bool threadDiscard(uint32 id);

/* Blocks the calling thread (parking with schedBlockCurrent + halt(), the
   same pattern pipeReadBlocking/pipeWriteBlocking use) until thread `id`
   has called threadExit, then returns its exit code and reaps it (frees
   the slot for reuse) in one step -- the same "wait for a child, get its
   status, reclaim it" contract as a real wait()/join(). Returns -1
   immediately, without blocking, for an invalid id (0, out of range, or a
   slot that is not currently a live thread) rather than parking forever on
   something that will never wake it. Joining the same id a second time (or
   from two different callers) is not supported: the first joiner reaps the
   slot, so a second joiner sees "no such thread" and gets -1. */
int64 threadJoin(uint32 id);

/* Non-blocking scan for waitpid(-1)-style "reap whichever child happens to
   already be dead, without caring which specific pid" (sysWait's waitAnyPid
   case, syscall.c/syscall.h; user/waitanytest.c is the live demo). Unlike
   threadJoin above this never calls halt() itself -- it can be (and is)
   invoked from inside the interrupt-context syscall handler, where parking
   directly would wedge the whole machine (see sysWait's own comment on why
   its specific-pid path does its own retry-syscall dance rather than just
   calling threadJoin unconditionally); the caller is responsible for
   actually blocking when this returns waitScanNoneDead.
   Scans every live slot for one whose parentId is `parentId`:
     - none exist at all (dead or alive) -> waitScanNoChildren, the real
       waitpid(-1) ECHILD case -- there is nothing to ever wait for, so the
       caller must not block.
     - at least one exists but none are threadDead yet -> waitScanNoneDead.
     - one is already threadDead -> waitScanFoundDead, with its own id and
       exit code written to *outId and *outCode; it is already fully reaped
       (threadReap already ran) by the time this returns, the same contract
       threadJoin(id) has for an already-dead target. Picks whichever
       matching dead slot has the lowest index if more than one child
       happens to be dead at once -- a real waitpid(-1) makes no promise
       about which one it returns in that case either. */
typedef enum { waitScanNoChildren, waitScanNoneDead, waitScanFoundDead } WaitScanResult;
WaitScanResult threadJoinAnyChild(uint32 parentId, uint32* outId, int64* outCode);

/* Terminates the CALLING thread with the given exit code (sysExit's own
   underlying implementation, syscall.c, and isr.c's uncaught-fatal-signal
   path). Also raises a real sigChld (signal.h/signal.c) against
   Thread.parentId above -- asynchronous notification, on top of (not a
   replacement for) threadJoin/sysWait's synchronous "block until this pid
   is Dead" contract: a parent with a sigChld handler installed and
   genuinely blocked elsewhere (see user/sigchldtest.c, which spawns
   user/sigchldchild.c) is woken through the same early-wake path
   threadWakeForSignal already gives any other signal, rather than only
   finding out the next time it happens to call sysWait. Harmless no-op on
   the sigRaise side if parentId is 0 (the idle thread, boot-time spawns'
   universal parent, see parentId's own comment) or already dead/unused
   (an orphan whose spawner is itself gone) -- sigRaise's own target
   validity check (signal.c) is what makes that safe, not anything special
   here. Also wakes the parent's OWN slot as a wait channel (on top of, not
   instead of, this slot's own channel below) -- the parking spot sysWait's
   waitAnyPid path (syscall.c) uses via schedBlockCurrent(currentThread()),
   so a parent genuinely blocked in userWaitAny wakes the instant ANY of
   its children exits, not just one it named specifically. */
void threadExit(uint64 code);

/* Exit code threadKill assigns its victim -- named after SIGKILL (9), the
   nearest real-world equivalent, though this kernel has no signal delivery
   at all: threadKill is a hard, unconditional termination, not something a
   target thread can catch, mask, or clean up after (see threadKill). */
#define threadKilledExitCode 9u

/* Unconditionally terminates thread `id` from OUTSIDE it -- the only way to
   ever get back a process that is genuinely stuck (e.g. blocked forever in
   a syscall-level pipe read with no writer ever coming, now that
   sysRead/sysWrite can really block, see pipe.h/vfs.h). Works on a thread
   in any of Runnable/Sleeping/Blocked: its state is simply set straight to
   threadDead with exitCode threadKilledExitCode, exactly as if it had
   called threadExit(threadKilledExitCode) itself, and whoever is (or later
   becomes) threadJoin()ing it wakes up and reaps it normally -- there is no
   "half-dead" state and no cleanup callback, since a mid-syscall thread has
   no user-mode cleanup path to run anyway. Returns false, changing nothing,
   for id 0 (idle), an out-of-range id, an already-Dead/never-created slot,
   or (implicitly, since only the CPU's one currently-executing thread is
   ever threadRunning) the caller's own id -- a syscall handler always runs
   as its OWN thread, which is Running, not Runnable/Sleeping/Blocked, at
   the moment it would try to kill itself, so self-kill is naturally
   rejected without a special-case check. Real termination (sysExit) is
   still how a thread should ever end itself. Also raises sigChld against
   the victim's parentId, exactly like threadExit does -- a killed child
   is still a dead child as far as its parent's SIGCHLD is concerned, the
   same way real kill(2)ing a child still generates SIGCHLD to its real
   parent. Also wakes the victim's parent's OWN slot as a wait channel,
   exactly like threadExit does -- a parent parked in userWaitAny does not
   care whether a child exited on its own or was killed, same as real
   waitpid(-1) does not distinguish the two either. */
bool threadKill(uint32 id);

/* Job control (signal.h): unconditionally moves thread `id` -- Runnable,
   Running (including the caller itself, unlike threadKill which naturally
   excludes self-targeting), Sleeping, or Blocked -- into threadStopped.
   Idempotent if `id` is already Stopped (matches real kill(pid, SIGSTOP)
   succeeding again on an already-stopped process). Safe on a
   Sleeping/Blocked thread for the same reason threadKill already is: every
   such thread is parked mid-retry-loop right after its own halt() (see
   pipeReadBlocking/pipeWriteBlocking, or the restart-syscall pattern
   syscall.c's sysRead/sysWrite/sysWait use), so simply not being picked by
   schedule() again until threadContinue moves it back is enough -- no
   wait-channel state is lost, since that retry loop re-checks its own
   condition from scratch every time it runs at all. Returns false,
   changing nothing, for id 0 (idle -- stopping it would wedge the whole
   machine the instant nothing else was runnable), an out-of-range id, or
   an already-Dead/never-created slot. */
bool threadStop(uint32 id);

/* Job control (signal.h): moves thread `id` from threadStopped back to
   threadRunnable. Returns false, changing nothing, if `id` is out of
   range or not currently Stopped (matches real SIGCONT delivered to a
   process that was never stopped -- a harmless no-op at the process-state
   level; signal.c's sigRaise still delivers the signal itself/runs a
   handler regardless of this return value). */
bool threadContinue(uint32 id);

/* Real Unix's interruptible sleep/EINTR: forces thread `id` from
   threadSleeping or threadBlocked straight to threadRunnable, early --
   before its sleep timer expires or its wait-channel condition is ever
   satisfied -- so the NEXT schedule() eventually resumes it, letting
   isr.c's deliverPendingSignals (which only ever runs at a return-to-ring3
   point) actually reach a thread that would otherwise never naturally
   produce one. Called only by signal.c's sigRaise, and only for a signal it
   has already determined would genuinely be delivered (not blocked, not
   ignored) if this thread resumed right now -- callers must not use this to
   wake a thread for an unrelated reason. Deliberately excludes
   threadStopped (only threadContinue, an explicit SIGCONT, may ever move a
   Stopped thread -- the same asymmetry schedWakeChannel already has, for
   the same reason: job control's "stopped means stopped" must survive an
   ordinary signal arriving too). Returns false, changing nothing, for an
   out-of-range id or any state other than Sleeping/Blocked (Runnable/
   Running need no forcing; Stopped/Dead/Unused have nothing to wake, or
   must not be woken this way at all). */
bool threadWakeForSignal(uint32 id);

void threadSleep(uint64 milliseconds);
void threadSetBrk(uint32 id, uint64 brk);
uint64 currentThreadSbrk(int64 increment);

/* Real alarm(2): schedules a real SIGALRM (signalnum.h's sigAlrm) against
   the CALLING thread only, `seconds` real seconds from now (converted to
   pitTicks() ticks via pitFrequency(), the same conversion threadSleep
   already uses for milliseconds) -- never another thread, exactly like
   POSIX alarm() has no target-pid argument at all. `seconds` == 0 cancels
   any alarm already pending for this thread instead of scheduling a new
   one, matching real alarm(0). Returns the number of whole seconds that
   remained on a PREVIOUSLY pending alarm for this thread (rounded up, so a
   deadline even one tick in the future still reports at least 1, exactly
   like real alarm() never reports "0 remaining" for one that has not
   actually fired yet), or 0 if none was pending -- the caller's own
   previous alarm is silently replaced (or cleared) either way, matching
   real alarm()'s "at most one pending alarm per process" contract. Actual
   delivery happens later, from schedule() (sched.c), once pitTicks()
   reaches the deadline this call records. */
uint64 threadSetAlarm(uint64 seconds);

/* Weighted-fair scheduling (CFS-lite): every thread starts at
   schedDefaultWeight. A thread with weight W accrues virtual runtime
   schedDefaultWeight/W times as fast as a default-weight thread each time
   it runs, so it is picked from the ready queue proportionally more often
   -- not given a longer timeslice, since this scheduler's granularity is
   already one timer tick. Returns false (no-op) for the same targets
   threadSetPgid already refuses -- the idle thread (id 0), an out-of-range
   id, or a slot that is not currently a live thread (Dead or Unused) --
   plus a zero weight, which used to be silently ignored (kmain.c-only
   caller never triggered it) before this became reachable from userspace
   through sysNice (syscall.c). Was a kernel-internal helper only used
   once, at boot, for workerA/workerB's own priority demo (kmain.c) --
   still is, but ALSO now the mechanism sysNice's syscall.c handler calls,
   so its own validation had to catch up to threadSetPgid's, not stay at
   the older, weaker "just check threadTotal" bound that let a userspace
   nice() call quietly reweight thread 0 (idle) or a slot no thread has
   ever occupied. */
bool threadSetPriority(uint32 id, uint32 weight);

/* Returns thread `id`'s current scheduling weight, or 0 (never a real
   weight -- threadSetPriority above always rejects weight 0, and every
   thread starts at schedDefaultWeight) for an out-of-range id or a slot
   that has never been a live thread (Unused) -- same "0 doubles as the
   error return, since it is never a valid one" shape threadPgid below
   has with threadInvalidId. A Dead-but-not-yet-reaped thread still
   reports its last real weight, exactly like threadPgid still reports
   its last real pgid for the same state -- see that function's own
   comment for why. */
uint32 threadPriority(uint32 id);
uint64 threadVruntime(uint32 id);

/* Returns thread `id`'s process group id (Thread.pgid above), or
   threadInvalidId for an out-of-range id or a slot that is not currently a
   live thread -- the same "invalid id sentinel doubles as an error return"
   convention threadInvalidId already has for allocation failure, safe here
   because a real pgid is always some other live thread's own id (never
   0xFFFFFFFF, since ids never reach that high, see threadCapacity). */
uint32 threadPgid(uint32 id);

/* Real setpgid(2)'s underlying mechanism: unconditionally sets thread
   `id`'s pgid to `pgid`, whatever value the caller resolved "0 means self"/
   "0 means use pid itself as the new group id" down to (that resolution is
   the SYSCALL layer's job, syscall.c's sysSetpgid -- mirroring how
   threadSetBrk/threadSetPriority/threadSetAlarm above already take a fully
   resolved value, not a raw syscall argument). Deliberately does not
   validate that `pgid` names any other real thread at all: this kernel has
   no session concept to check membership against (real setpgid(2) would
   reject joining a group outside the caller's own session), so -- like
   several other omissions this codebase documents honestly rather than
   fakes -- any uint32 is accepted, exactly as if every process were in one
   giant session. Returns false, changing nothing, for id 0 (idle -- it has
   no job-control identity to reassign) or an out-of-range/Dead/Unused id. */
bool threadSetPgid(uint32 id, uint32 pgid);

/* Real kill(-pgid, SIGKILL)-shaped bulk termination: calls threadKill on
   every currently live thread (Runnable/Sleeping/Blocked/Stopped, per
   threadKill's own accepted set) whose pgid equals `pgid`, one at a time,
   through the ordinary single-target threadKill above -- so its own
   per-target SIGCHLD-to-parent notification and wait-channel wakes already
   apply per victim exactly as they would for a plain, non-group kill,
   rather than this function duplicating any of that. A pgid whose only
   member is the CALLER's own currently-Running thread naturally kills
   nobody (threadKill excludes Running, i.e. self, by construction, the
   same "self-kill via plain kill(2) is not really different" reasoning
   userKill's own doc comment already gives) without needing a special
   case here. Returns false, killing nobody, if no currently live thread
   has this pgid at all -- the real ESRCH case for kill(-pgid, ...) with no
   matching group. */
bool threadKillGroup(uint32 pgid);

/* Real getcwd(2)/chdir(2)'s underlying per-thread state (Thread.cwd above):
   always the CALLING thread's own directory, never another pid's -- unlike
   threadSetPgid, there is no `id` parameter here at all, since POSIX's
   chdir() has no cross-process form the way setpgid(2) does for a child
   still between fork() and exec(). currentThreadSetCwd does no validation
   of its own (mirrors threadSetPgid's own "that is the syscall layer's
   job" split) -- vfs.c's vfsChdir is what actually confirms `path` names a
   real, already-resolved-to-absolute directory before ever calling this;
   a `path` longer than fsPathMax-1 is rejected here (false, unchanged)
   since it could never round-trip back out through currentThreadCwd(). */
const char* currentThreadCwd(void);
bool currentThreadSetCwd(const char* path);

/* Real getuid(2)/setuid(2)'s underlying per-thread state (Thread.uid
   above): always the CALLING thread's own identity, mirroring
   currentThreadCwd's own "no id parameter, no cross-process form" shape
   exactly (POSIX setuid() has no cross-process form either -- a process can
   only ever change its OWN credential). currentThreadSetUid enforces the
   real privilege rule itself (this kernel's syscall layer does no
   additional validation of its own, unlike currentThreadSetCwd's split):
   uid 0 ("root") may become anyone; any other uid may only pass its own
   current value back in (a no-op that still reports success, matching real
   setuid(getuid())), and any other target is rejected, state unchanged.
   There is deliberately no way back to uid 0 once dropped, short of
   already being uid 0 at the time of the call -- the same one-way
   privilege-drop shape a real setuid daemon relies on. */
uint32 currentThreadUid(void);
bool currentThreadSetUid(uint32 newUid);

/* Real getgid(2)/setgid(2)'s underlying per-thread state (Thread.gid above)
   -- same "no id parameter, no cross-process form" shape as currentThreadUid
   immediately above. currentThreadSetGid's privilege gate is the CALLING
   thread's own currentThreadUid() (== 0), not its gid -- matching real
   setgid(2), whose privilege check is CAP_SETGID (effectively "is this
   process root"), never based on the gid being changed itself. A
   non-privileged thread may only pass its own current gid back in (a
   no-op success, matching real setgid(getgid())); any other target is
   rejected, state unchanged. */
uint32 currentThreadGid(void);
bool currentThreadSetGid(uint32 newGid);

/* Real getenv(3)/setenv(3)'s underlying per-thread state (Thread.env
   above): always the CALLING thread's own table, mirroring
   currentThreadCwd/currentThreadSetCwd's own "no id parameter, no
   cross-process form" shape exactly (POSIX setenv() has no cross-process
   form either). currentThreadGetEnv returns a pointer straight into the
   matching slot's own VALUE half (never a copy) or nullptr if `name` has
   no entry -- that pointer stays valid only until the next
   currentThreadSetEnv call on the SAME name, which may overwrite the slot
   in place. currentThreadSetEnv overwrites an existing entry with a
   matching `name` in place, or appends a new one if a slot is still free
   under envMaxVars; returns false, table unchanged, if `name`+'='+`value`
   would not fit in envEntryMax bytes, or if `name` is new and every slot
   is already taken. */
const char* currentThreadGetEnv(const char* name);
bool currentThreadSetEnv(const char* name, const char* value);

/* Real unsetenv(3): removes the calling thread's own entry named `name`,
   if any, compacting the table (memmove of every later slot down by one)
   so envCount and currentThreadEnvEntry's own index space never develop a
   hole -- unlike currentThreadSetEnv's in-place overwrite, this changes
   WHICH index every later entry lives at. Returns true if an entry was
   actually removed, false (table unchanged) if `name` had no entry --
   matching real unsetenv(3), which is a no-op success either way, but the
   bool lets a caller (shell.c's `unset` builtin) tell the difference if it
   ever wants to. */
bool currentThreadUnsetEnv(const char* name);

/* Iterates the calling thread's own environment table for listing
   (shell.c's `env` builtin): currentThreadEnvCount() is how many of the
   envMaxVars slots are actually in use; currentThreadEnvEntry(index)
   returns the raw "NAME=VALUE" string stored at that slot, or nullptr for
   an out-of-range index. */
int currentThreadEnvCount(void);
const char* currentThreadEnvEntry(int index);

/* Copies up to threadNameMax-1 bytes of `name` into thread `id`'s own name
   buffer (truncating, always nul-terminated). Used by exec() (syscall.c) so
   a process's visible identity (shell `ps`, the exit log) reflects the new
   image after it replaces the old one -- the same way a real process's
   name changes across execve(). Invalid id is a silent no-op. */
void threadSetName(uint32 id, const char* name);

/* Base of thread `id`'s user-memory slot (see Thread.userSlotBase above),
   or 0 for an out-of-range id or a kernel thread that owns no user slot.
   Used by exec() (syscall.c) to find the OLD slot to reclaim before it
   overwrites its own image with a new one. */
uint64 threadUserSlotBase(uint32 id);

/* Records which elfSlotSize-sized slot thread `id` currently occupies.
   Only exec() (syscall.c) needs this directly -- threadCreateUser sets it
   itself from the entry point it is given. Invalid id is a silent no-op. */
void threadSetUserSlotBase(uint32 id, uint64 base);

/* Wait-queue primitives ("sleep on a channel", xv6-style). A channel is any
   stable address the caller controls (e.g. the address of the resource being
   waited on) - it is never dereferenced, only compared for identity.

   schedBlockCurrent marks the calling thread as blocked on a channel; it does
   NOT itself give up the CPU (this scheduler is timer-preemptive, not
   cooperative - callers park with halt() the same way threadSleep callers
   already do, see kmain.c). schedWakeChannel moves every thread blocked on a
   matching channel back to runnable; it is always safe to call even when no
   thread is waiting (no-op) and must be called by whichever side of a
   producer/consumer relationship changes the condition the other side is
   waiting on. */
void schedBlockCurrent(const void* channel);
void schedWakeChannel(const void* channel);

/* The one wait channel every not-yet-satisfied sysPoll call blocks on (see
   Thread.pollActive's own comment above) -- any code path that can make a
   previously-not-ready fd become ready (pipe.c's pipeWrite/pipeRead
   freeing space, pipeCloseRead/pipeCloseWrite breaking a pipe,
   keyboard.c's keyboardHandleScancode enqueueing a keystroke) calls
   schedWakeChannel(schedPollChannel()) in addition to whatever
   fd-specific channel it already wakes, so every blocked poller gets a
   chance to re-check its own fd list. Always returns the same address for
   the life of the kernel -- not tied to any particular fd or pipe, unlike
   pipeChannel/keyboardReaderChannel. */
const void* schedPollChannel(void);

uint32 threadCount(void);
Thread* threadAt(uint32 index);
Thread* currentThread(void);
uint32 currentThreadId(void);

#endif
