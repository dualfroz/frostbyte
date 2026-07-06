#include "sched.h"
#include "isr.h"
#include "heap.h"
#include "string.h"
#include "ports.h"
#include "gdt.h"
#include "tss.h"
#include "pit.h"
#include "console.h"
#include "paging.h"
#include "vfs.h"
#include "prioq.h"
#include "elf.h"
#include "keyboard.h"
#include "pipe.h"
#include "pidfd.h"
#include "signal.h"

/* Bumped from 24: boot now spawns 22 kernel/user threads before the
   scheduler even starts (kmain.c) -- several of which (workerA/workerB,
   schedVerify, pipeProducer/pipeConsumer) park in an infinite halt() loop
   rather than ever calling threadExit, so their slots are never freed for
   reuse -- plus a handful more spawned at runtime by those boot programs
   themselves (fifomaster's fiforeader/fifowriter, killtest's killvictim,
   proctest's spawntarget/argtest) that can be alive at the same time as
   their still-running parent. 24 was exactly enough before namedreader/
   namedwriter's fifo-rendezvous demo added two more boot-time processes;
   with them, every one of those later spawns (fifomaster's, killtest's,
   proctest's own) started failing with the table genuinely full. The
   free-slot reuse machinery below (allocSlot/threadReap) also needs
   headroom above whatever boot needs to demonstrate exhaustion+reuse in
   tests without permanently eating into threads real boot needs.
   consolereader1/2+their injector and sigtest+its two short-lived,
   promptly-reaped children (signal.h's demo) landed since without needing
   another bump -- 32 held across dozens of stress runs, but with exactly
   zero slots of headroom left over: boot-time spawns alone (kmain.c) had
   already reached the full 32 the moment sigtest's own spawnUser call
   returned, before any of killtest's/fifomaster's/proctest's own
   runtime-spawned children (killvictim, fifowriter, spawntarget, argtest)
   ever got a chance to exist. That razor-thin margin turned into a real,
   observed race the moment jobtest+jobtarget (job control's live demo,
   signal.h) landed as two more boot/runtime processes: every one of those
   OTHER programs' children now had to wait for exactly one freed slot
   (killvictim's, once threadReap'd) to fight over, and fifomaster's
   fifowriter won that fight at the exact moment killtest was mid-way
   through its own "kill an already-reaped pid must fail" check --
   recycling killvictim's just-freed pid number into fifowriter's very much
   alive one, so killtest's second userKill(victim) call genuinely
   succeeded (against fifowriter, not "nothing") when it expected -1.
   Not a scheduling-timing flake (see ci-test.sh's documented host-load
   flakiness elsewhere): a deterministic consequence of table pressure,
   reproduced identically across repeated runs until this bump. Restored
   real headroom (bump to 40, not just the 2 jobtest/jobtarget need) so a
   future addition does not immediately re-create the same zero-margin
   situation. nesttest/nesttarget and eintrtest/eintrtarget (4 more
   boot/runtime processes, signal.h's nested-delivery and early-wake demos)
   fit under 40 with zero bump needed -- confirmed as the exact high-water
   mark (`[sched] 40 threads total`), i.e. zero slots of headroom left
   over again. restarttest/restarttarget (2 more, signal.h's SA_RESTART/
   EINTR demo) would immediately hit the same razor-thin-margin class of
   bug FALA 16's maxThreads bump above already fixed once, so bumped ahead
   of that this time (40 -> 48) instead of waiting to observe it.
   Confirmed as the exact high-water mark yet again once restarttest/
   restarttarget actually landed (`[sched] 48 threads total`, checked live
   before touching anything this fala) -- zero slots of headroom left over,
   same story as 40 was before it. masktest/masktarget (2 more, signal.h's
   sa_mask-defers-then-delivers live demo) would immediately re-create the
   exact same class of bug, so bumped ahead of that again (48 -> 56)
   instead of waiting to observe it, same reasoning as every bump above.
   Confirmed as the exact high-water mark yet again once masktest/
   masktarget landed (`[sched] 56 threads total`, checked live before
   touching anything this fala) -- zero slots of headroom left over.
   sigchldtest/sigchldchild (2 more, sched.c's new parentId/SIGCHLD-on-exit
   live demo) would immediately re-create the exact same class of bug, so
   bumped ahead of that again (56 -> 64) instead of waiting to observe it,
   same reasoning as every bump above. Confirmed as the exact high-water
   mark yet again once sigchldtest/sigchldchild landed (`[sched] 64 threads
   total`, checked live before touching anything this fala) -- zero slots
   of headroom left over. waitanytest/waitanyfast/waitanyslow (3 more,
   sched.c's new threadJoinAnyChild/userWaitAny live demo) would
   immediately re-create the exact same class of bug, so bumped ahead of
   that again (64 -> 72) instead of waiting to observe it, same reasoning
   as every bump above. Confirmed as the exact high-water mark yet again
   once waitanytest/waitanyfast/waitanyslow landed (`[sched] 72 threads
   total`, checked live before touching anything this fala) -- zero slots
   of headroom left over. stopchldtest/stopchldtarget (2 more, sched.c's
   new threadStop/threadContinue SIGCHLD-on-stop/SIGCHLD-on-continue live
   demo) would immediately re-create the exact same class of bug, so
   bumped ahead of that again (72 -> 80) instead of waiting to observe it,
   same reasoning as every bump above. Confirmed as the exact high-water
   mark yet again once stopchldtest/stopchldtarget landed (`[sched] 80
   threads total`, checked live before touching anything this fala) --
   zero slots of headroom left over. nodefertest (1 more, signal.h's new
   sigFlagNoDefer live demo -- unlike every pair above, self-contained, no
   spawned counterpart) would immediately re-create the exact same class
   of bug, so bumped ahead of that again (80 -> 88) instead of waiting to
   observe it, same reasoning as every bump above. alarmtest/alarmdefault
   (2 more, sched.c's new threadSetAlarm/schedule()-expiry-scan real
   SIGALRM live demo) would immediately re-create the exact same class of
   bug, so bumped ahead of that again (88 -> 96) instead of waiting to
   observe it, same reasoning as every bump above. siginfotest/
   siginfotarget (2 more, signal.h's new sigFlagSigInfo/SigInfo live demo)
   would immediately re-create the exact same class of bug, so bumped ahead
   of that again (96 -> 104) instead of waiting to observe it, same
   reasoning as every bump above. Confirmed as the exact high-water mark
   yet again once siginfotest/siginfotarget landed (`[sched] 104 threads
   total`, checked live before touching anything this fala) -- zero slots
   of headroom left over. pgroupfg/pgroupbg (2 more, sched.c's new pgid/
   process-group live demo, plus a third kernel-only injector thread,
   kmain.c's pgroupInjectorThread) would immediately re-create the exact
   same class of bug, so bumped ahead of that again (104 -> 112) instead of
   waiting to observe it, same reasoning as every bump above. Confirmed as
   the exact high-water mark yet again once pgroupfg/pgroupbg landed
   (`[sched] 112 threads total`, checked live before touching anything this
   fala) -- zero slots of headroom left over. jobctlfg/jobctlbg (2 more,
   jobctl.c's new generic by-path spawn+real-job-control helper, loaded on
   demand by the shell's own `NAME [ARGS] [&]` fallback rather than eagerly
   at boot like everything above -- the first two user programs in this
   kernel never once named in a kmain.c spawnUser call), plus a third
   kernel-only driver thread (kmain.c's jobctlDriverThread, which types
   commands at the real shell through the same injected-keystroke mechanism
   pgroupInjectorThread already uses) would immediately re-create the exact
   same class of bug, so bumped ahead of that again (112 -> 120) instead of
   waiting to observe it, same reasoning as every bump above. polltest (1
   more, boot-time driver for the new sysPoll multiplexed-fd wait) plus
   pollwriter (1 more, runtime-spawned by polltest itself, mirroring
   alarmtest/alarmdefault's shape) would immediately re-create the exact
   same class of bug, so bumped ahead of that again (120 -> 128) instead of
   waiting to observe it, same reasoning as every bump above. pipesrc/
   pipesink (2 more, jobctl.c's new real shell pipeline demo, loaded on
   demand exactly like jobctlfg/jobctlbg rather than boot-time spawnUser),
   plus a third kernel-only driver thread (kmain.c's pipelineDriverThread,
   which types the pipeline command at the real shell through the same
   injected-keystroke mechanism jobctlDriverThread already uses) would
   immediately re-create the exact same class of bug, so bumped ahead of
   that again (128 -> 136) instead of waiting to observe it, same reasoning
   as every bump above. redirwriter (1 more, jobctl.c's new file
   redirection demo, run TWICE by kmain.c's redirDriverThread against the
   same on-demand-loaded binary -- but each run is its own spawn+exit, so
   only ONE of its process-table slots is ever live at a time) plus
   redirreader (1 more, same on-demand-loading pattern as pipesrc/pipesink
   above), plus a third kernel-only driver thread (kmain.c's
   redirDriverThread, which types the redirected commands at the real
   shell through the same injected-keystroke mechanism jobctlDriverThread/
   pipelineDriverThread already use) would immediately re-create the exact
   same class of bug, so bumped ahead of that again (136 -> 144) instead of
   waiting to observe it, same reasoning as every bump above. stderrwriter
   (1 more, jobctl.c's new stderr (fd 2) redirection demo, run TWICE by
   kmain.c's stderrDriverThread against the same on-demand-loaded binary --
   the exact same "only ONE of its process-table slots is ever live at a
   time" shape redirwriter already has for `>`/`>>`) plus stderrreader (1
   more, same on-demand-loading pattern as redirreader above), plus a third
   kernel-only driver thread (kmain.c's stderrDriverThread, which types the
   `2>`/`2>>`-redirected commands at the real shell through the same
   injected-keystroke mechanism redirDriverThread already uses) would
   immediately re-create the exact same class of bug, so bumped ahead of
   that again (144 -> 152) instead of waiting to observe it, same reasoning
   as every bump above. selfstoptest (boot-time spawned, self-directed
   SIGSTOP live demo) plus selfstoptarget (1 more, runtime-spawned by
   selfstoptest, the same *test / *target relationship jobtest/jobtarget
   already have) are two more concurrently live threads, so bumped again
   (152 -> 160) for the same headroom reason as every bump above. herestest
   (1 more, jobctl.c's new here-string `<<<` stdin demo, on-demand-loaded
   exactly like redirreader/stderrreader above -- no writer counterpart
   binary at all, since the "writer" is the shell's own kernel thread, not
   another spawned process) plus a third kernel-only driver thread (kmain.c's
   hereDriverThread, which types the here-string command at the real shell
   through the same injected-keystroke mechanism redirDriverThread/
   stderrDriverThread already use) would immediately re-create the exact
   same class of bug, so bumped ahead of that again (160 -> 168) instead of
   waiting to observe it, same reasoning as every bump above. hcat (1 more,
   jobctl.c's new jobctlSpawnPipedHereString demo -- a here-string stdin AND
   a piped stdout on the same process at once, on-demand-loaded exactly like
   herestest above) plus hsink (1 more, same on-demand-loading pattern as
   pipesink above), plus a third kernel-only driver thread (kmain.c's
   hereStringPipelineDriverThread, which types the here-string-into-pipeline
   command at the real shell through the same injected-keystroke mechanism
   pipelineDriverThread/hereDriverThread already use) would immediately
   re-create the exact same class of bug, so bumped ahead of that again
   (168 -> 176) instead of waiting to observe it, same reasoning as every
   bump above. pipecat (1 more, shell.c's new runPipelineStages demo -- the
   first process in this kernel with two DIFFERENT real inter-process pipes
   on its own stdio at once, a genuine middle pipeline stage rather than a
   here-string-plus-pipe like hcat above), plus a third kernel-only driver
   thread (kmain.c's threeStagePipelineDriverThread, which types the
   three-stage pipeline command at the real shell through the same
   injected-keystroke mechanism pipelineDriverThread/hereStringPipelineDriverThread
   already use) would immediately re-create the exact same class of bug, so
   bumped ahead of that again (176 -> 184) instead of waiting to observe it,
   same reasoning as every bump above. cwdtest (boot-time spawned, real
   per-process working directory live demo -- sched.h's new Thread.cwd)
   plus cwdtarget (1 more, runtime-spawned by cwdtest itself, the same
   *test / *target relationship jobtest/jobtarget already have) are two
   more concurrently live threads, so bumped again (184 -> 192) for the
   same headroom reason as every bump above. envprint (loaded on demand by
   the real shell, like jobctlfg/hcat/pipecat above, never boot-spawned)
   plus a kernel-only driver thread (kmain.c's envDriverThread, which types
   `export`/`envprint`/`env` at the real shell through the same
   injected-keystroke mechanism every driver thread above already uses)
   would immediately re-create the exact same class of bug, so bumped
   ahead of that again (192 -> 200) instead of waiting to observe it, same
   reasoning as every bump above. nicetest (boot-time spawned, real
   userspace-triggered sysNice/sysGetPriority live demo) plus nicetarget
   (1 more, runtime-spawned by nicetest itself while sleeping so it can be
   reniced while blocked, the same *test / *target relationship
   jobtest/jobtarget and cwdtest/cwdtarget already have) are two more
   concurrently live threads, so bumped again (200 -> 208) for the same
   headroom reason as every bump above. date (boot-time spawned, real
   sysTime/rtc.c live demo, fully synchronous like nicetest with no
   runtime-spawned partner of its own) is one more, so bumped again
   (208 -> 216) for the same headroom reason as every bump above. timetest
   (boot-time spawned, real SFS inode mtime/ctime live demo through userOpen/
   userWrite/userStat/userLink, fully synchronous like nicetest/date with no
   runtime-spawned partner) is one more, so bumped again (216 -> 224) for the
   same headroom reason as every bump above. lsDriverThread (kmain.c, boot-
   time kernel thread that types `mkdir`/`echo`/`ls -l` at the real shell
   for shell.c's new generalized `ls` builtin) is one more, so bumped again
   (224 -> 232) for the same headroom reason as every bump above.
   andOrDriverThread (kmain.c, boot-time kernel thread that types
   `false && echo X || echo ao2` at the real shell for shell.c's new
   `&&`/`||` conditional chaining) is one more, so bumped again
   (232 -> 240) for the same headroom reason as every bump above. rmtest
   (boot-time spawned, real vfsRmdir/sysRmdir unlink(2)-vs-rmdir(2) split
   live demo on both tmpfs and SFS, fully synchronous like nicetest/date/
   timetest with no runtime-spawned partner) is one more, so bumped again
   (240 -> 248) for the same headroom reason as every bump above. permtest
   (boot-time spawned, real chmod(2)/owner-rwx-permission-bits live demo)
   plus the one short-lived runtime-spawned copy of /bin/false it chmods and
   spawns to prove the exec-permission check, are two more concurrently
   live threads, so bumped again (248 -> 256) for the same headroom reason
   as every bump above. dirpermtest (boot-time spawned, real directory-mode
   enforcement live demo -- vfsMkdir/resolveOpen/vfsUnlink/vfsRmdir's new
   parent-write and ancestor-search checks, lookupNode/lookupFrom's new
   exec-on-traversal check -- fully synchronous like identtest with no
   runtime-spawned partner) is one more, so bumped again (256 -> 264) for
   the same headroom reason as every bump above. grouptest (boot-time
   spawned, real group-credential/vfsPermitted-group-branch live demo plus
   the two short-lived runtime-spawned copies of /bin/false it group-execs
   to prove the exec-permission check across matching/mismatched groups)
   are two more concurrently live threads, so bumped again (264 -> 272) for
   the same headroom reason as every bump above. pidfdtest (boot-time
   spawned, real pidfd.h live demo through sysPidfdOpen/sysPidfdSend/
   sysPidfdWait) plus the one runtime-spawned copy of /bin/killvictim it
   reuses as its target (same proven "genuinely stuck" shape killtest.c
   already established) are two more concurrently live threads, so bumped
   again (272 -> 280) for the same headroom reason as every bump above.
   mmaptest (boot-time spawned, real mmap(2)/munmap(2) demand-paging live
   demo through sysMmap/sysMunmap -- anonymous demand-zero, file-backed
   read-through, and a real SIGSEGV proven against a PROT_READ-only
   mapping) plus the one runtime-spawned copy of /bin/mmapprotvictim it
   uses to prove that last case are two more concurrently live threads, so
   bumped again (280 -> 288) for the same headroom reason as every bump
   above. */
#define maxThreads 288
#define threadStackSize 16384

/* Virtual-runtime charge for one timer tick's worth of execution at
   schedDefaultWeight; scaled by schedDefaultWeight/weight for other
   weights (see threadSetPriority). Purely an internal unit -- only ever
   compared against other vruntime values, never against wall-clock time. */
#define schedTickCharge 1000000ull

static Thread threads[maxThreads];
static uint32 threadTotal;
static uint32 currentIndex;
static bool schedulerActive;

/* allocSlot's own round-robin cursor -- see that function's doc comment
   for why this exists at all, not just "the lowest free index" the way
   threadTotal's own high-water-mark growth already reads left to right. */
static uint32 nextSlotHint = 1;

/* Thread.generation's own monotonic source (see that field's doc comment,
   sched.h) -- never 0 (0 is reserved to mean "this slot is currently
   unoccupied", the same convention pipe.c's nextPipeIncarnation already
   uses for the identical reason), never reused, incremented once per
   threadCreate/threadCreateUser regardless of which slot the new thread
   lands in. */
static uint32 nextThreadGeneration = 1;

/* Bounded copy into a Thread's own name buffer (see threadNameMax /
   sched.h) -- never aliases the caller's storage, since threadCreateUser
   is called from syscall.c with a path that lives in a stack-local buffer
   for the duration of one syscall. */
static void copyThreadName(Thread* thread, const char* name)
{
    uint64 len = strlen(name);
    if (len >= threadNameMax)
        len = threadNameMax - 1;
    memcpy(thread->name, name, len);
    thread->name[len] = 0;
}

void threadSetName(uint32 id, const char* name)
{
    if (id >= threadTotal)
        return;
    copyThreadName(&threads[id], name);
}

void schedInit(void)
{
    threadTotal = 1;
    currentIndex = 0;

    threads[0].id = 0;
    threads[0].parentId = 0;
    /* Reserved, permanently: idle is never reaped/recreated, and pid 0 is
       already rejected as a pidfd/kill/wait target everywhere else, so it
       never needs a real incarnation of its own (see Thread.generation's
       own doc comment). */
    threads[0].generation = 0;
    threads[0].pgid = 0;
    threads[0].uid = 0;
    threads[0].gid = 0;
    threads[0].cwd[0] = '/';
    threads[0].cwd[1] = 0;
    threads[0].envCount = 0;
    copyThreadName(&threads[0], "idle");
    threads[0].state = threadRunning;
    threads[0].rsp = 0;
    threads[0].stackBase = 0;
    threads[0].kernelStackTop = 0;
    threads[0].sleepUntil = 0;
    threads[0].brk = 0;
    threads[0].vruntime = 0;
    threads[0].exitCode = 0;
    threads[0].weight = schedDefaultWeight;
    threads[0].everRan = true;
    threads[0].waitChannel = nullptr;
    vfsFdInit(threads[0].fds);
}

/* Finds a slot for a new thread: a threadUnused slot left by a prior
   threadReap, or (if none is free) grows the high-water mark by one as
   long as there is room under maxThreads. Returns threadInvalidId,
   touching nothing, if the table is completely full -- the caller must not
   write into threads[] in that case (this is the bounds-check the original
   version of this table was missing: threadTotal used to grow unconditionally
   on every create, silently writing past the array once 16 threads existed).
   Scans round-robin from nextSlotHint (wrapping through 1..threadTotal-1)
   rather than always restarting from the lowest index: a real,
   reproducibly observed bug this fala's own gate caught fresh on BOTH
   killtest.c (a pre-existing, years-old check) and the new nicetest.c --
   "kill/nice a pid this process itself just userWait()-reaped must fail"
   occasionally failed for real, because some COMPLETELY UNRELATED thread
   elsewhere in the system (in the reproduced case, jobctlfg being loaded
   by the shell) called threadCreate/threadCreateUser in the tiny window
   between the reap and the recheck and got handed that exact just-freed
   LOW index straight back, since the old scan always tried index 1 first.
   That is table pressure racing against a single process's own two
   syscalls, not a scheduling-timing flake -- ci-test.sh's needle for it
   (killtest: kill-ok) had simply never been unlucky enough to hit it
   before this fala's elevated host load made the race common enough to
   reproduce in a handful of runs. A round-robin cursor does not make the
   race impossible (a table sustained at 100% occupancy for a full sweep
   could still wrap back around to a freshly-freed slot), but with this
   project's real, generous headroom margins (maxThreads far above actual
   concurrent occupancy at any point in the historical record, sched.h's
   own bump history) it means dozens-to-hundreds of OTHER allocations have
   to happen before the cursor can return to any one just-freed slot --
   the same practical-elimination-via-headroom shape every other table-
   pressure fix in this file's history already has, not a new kind of
   guarantee. A real generation/incarnation counter on pid itself (the
   pipeIncarnation/sfsGeneration shape vfs.c/sfs.c already use for pipes
   and the on-disk filesystem) would close it completely regardless of
   occupancy, but reshaping what a "pid" IS everywhere kill/wait/sigSend/
   setpgid/nice already accept it as a raw table index is the large,
   deferred, cross-cutting change this project's own history has flagged
   many times over (killtest.c's own doc comment, frostbyte-budowa.md) --
   out of scope for this fix.

   That generation counter now exists (Thread.generation, sched.h; this
   file's own nextThreadGeneration below) -- but deliberately NOT wired
   into kill/wait/sigSend/setpgid/nice's raw pid argument, matching real
   Linux's own resolution of this exact, well-known race: pidfd_open(2)
   added a SEPARATE, opt-in handle type rather than ever making a plain
   pid argument itself generation-checked. See pidfd.h for the additive
   primitive built on Thread.generation, and schedTest.c's pidfd tests for
   a deterministic (create/discard/recreate, no live scheduling involved)
   reproduction of exactly the stale-handle-hits-an-unrelated-live-thread
   scenario killtest.c's own comment warns about but never guards against. */
static uint32 allocSlot(void)
{
    if (threadTotal > 1) {
        uint32 start = (nextSlotHint >= 1 && nextSlotHint < threadTotal) ? nextSlotHint : 1;
        uint32 i = start;
        do {
            if (threads[i].state == threadUnused) {
                nextSlotHint = i + 1;
                return i;
            }
            i = (i + 1 < threadTotal) ? i + 1 : 1;
        } while (i != start);
    }
    if (threadTotal < maxThreads) {
        uint32 slot = threadTotal++;
        nextSlotHint = slot + 1;
        return slot;
    }
    return threadInvalidId;
}

/* Thread.pgid's own creation-time default (see that field's doc comment,
   sched.h): a thread created while the idle thread (id 0) is "current" can
   only be one of kmain.c's boot-time calls (parentId's own comment already
   establishes idle never issues a syscall, so it can never be "current"
   again once real processes exist) -- exactly like a real init spawning
   each service into its own job, it becomes its own new group leader.
   Anything else is a genuine runtime child (syscall.c's doSpawn) of a
   currently-Running parent, and inherits that parent's pgid, matching real
   fork()'s own default before any setpgid ever runs. */
static uint32 initialPgid(uint32 newId)
{
    if (currentIndex == 0)
        return newId;
    return threads[currentIndex].pgid;
}

/* Thread.uid's own creation-time default, same "is the idle thread
   currently running" test as initialPgid immediately above but with
   pgid's own conclusion inverted: a boot-time thread starts as uid 0
   ("root"), like a real init process running as the superuser, rather than
   becoming its own new anything; a genuine runtime child instead INHERITS
   its parent's uid outright (not derived from newId at all), matching real
   fork()'s own default before any setuid ever runs -- the same "copy the
   parent's value" shape initialCwd/initialEnv below already use for their
   own fields, just a scalar instead of a buffer so it returns a value
   like initialPgid does. */
static uint32 initialUid(uint32 newId)
{
    (void)newId;
    if (currentIndex == 0)
        return 0;
    return threads[currentIndex].uid;
}

/* Thread.gid's own creation-time default, byte-for-byte the same shape as
   initialUid immediately above: a boot-time thread starts at gid 0, a
   genuine runtime child INHERITS its parent's gid outright. */
static uint32 initialGid(uint32 newId)
{
    (void)newId;
    if (currentIndex == 0)
        return 0;
    return threads[currentIndex].gid;
}

/* Thread.cwd's own creation-time default, mirroring initialPgid's own
   reasoning immediately above exactly (same "is the idle thread currently
   running" test, same conclusion for the same reason): a boot-time thread
   starts at the filesystem root, like a real init process; a genuine
   runtime child instead gets a byte-for-byte COPY of its parent's cwd,
   matching real fork()'s own copy-not-share semantics -- writes into
   threads[newId].cwd directly (rather than returning a value the way
   initialPgid does) since a path is a buffer, not a scalar. */
static void initialCwd(uint32 newId)
{
    if (currentIndex == 0) {
        threads[newId].cwd[0] = '/';
        threads[newId].cwd[1] = 0;
        return;
    }
    memcpy(threads[newId].cwd, threads[currentIndex].cwd, fsPathMax);
}

/* Thread.env's own creation-time default, mirroring initialCwd's own
   reasoning immediately above exactly (same "is the idle thread currently
   running" test, same conclusion for the same reason): a boot-time thread
   starts with an empty table, like a real init process before anything
   ever calls setenv(); a genuine runtime child instead gets a byte-for-byte
   COPY of its parent's whole table (both the entries and envCount), never
   a shared pointer. */
static void initialEnv(uint32 newId)
{
    if (currentIndex == 0) {
        threads[newId].envCount = 0;
        return;
    }
    memcpy(threads[newId].env, threads[currentIndex].env, sizeof(threads[newId].env));
    threads[newId].envCount = threads[currentIndex].envCount;
}

uint32 threadCreate(const char* name, ThreadEntry entry)
{
    uint32 slot = allocSlot();
    if (slot == threadInvalidId)
        return threadInvalidId;

    uint64 stackBase = (uint64)kmalloc(threadStackSize);
    uint64 stackTop = (stackBase + threadStackSize) & ~15ull;

    Registers* frame = (Registers*)(stackTop - sizeof(Registers));
    memset(frame, 0, sizeof(Registers));
    frame->rip = (uint64)entry;
    frame->cs = kernelCodeSelector;
    frame->rflags = 0x202;
    frame->rsp = stackTop;
    frame->ss = kernelDataSelector;

    threads[slot].id = slot;
    threads[slot].generation = nextThreadGeneration++;
    threads[slot].parentId = currentIndex;
    threads[slot].pgid = initialPgid(slot);
    threads[slot].uid = initialUid(slot);
    threads[slot].gid = initialGid(slot);
    initialCwd(slot);
    initialEnv(slot);
    copyThreadName(&threads[slot], name);
    threads[slot].state = threadRunnable;
    threads[slot].stackBase = stackBase;
    threads[slot].kernelStackTop = stackTop;
    threads[slot].rsp = (uint64)frame;
    threads[slot].sleepUntil = 0;
    threads[slot].alarmAt = 0;
    threads[slot].pollActive = false;
    threads[slot].pollDeadline = 0;
    threads[slot].brk = 0;
    threads[slot].vruntime = 0;
    threads[slot].exitCode = 0;
    threads[slot].weight = schedDefaultWeight;
    threads[slot].everRan = false;
    threads[slot].waitChannel = nullptr;
    threads[slot].userSlotBase = 0;
    vfsFdInit(threads[slot].fds);
    sigResetState(&threads[slot]);

    return slot;
}

uint32 threadCreateUser(const char* name, uint64 entry, uint64 userStackTop)
{
    uint32 slot = allocSlot();
    if (slot == threadInvalidId)
        return threadInvalidId;

    uint64 stackBase = (uint64)kmalloc(threadStackSize);
    uint64 kernelStackTop = (stackBase + threadStackSize) & ~15ull;

    Registers* frame = (Registers*)(kernelStackTop - sizeof(Registers));
    memset(frame, 0, sizeof(Registers));
    frame->rip = entry;
    frame->cs = userCodeSelector | 3;
    frame->rflags = 0x202;
    frame->rsp = userStackTop;
    frame->ss = userDataSelector | 3;

    threads[slot].id = slot;
    threads[slot].generation = nextThreadGeneration++;
    threads[slot].parentId = currentIndex;
    threads[slot].pgid = initialPgid(slot);
    threads[slot].uid = initialUid(slot);
    threads[slot].gid = initialGid(slot);
    initialCwd(slot);
    initialEnv(slot);
    copyThreadName(&threads[slot], name);
    threads[slot].state = threadRunnable;
    threads[slot].stackBase = stackBase;
    threads[slot].kernelStackTop = kernelStackTop;
    threads[slot].rsp = (uint64)frame;
    threads[slot].sleepUntil = 0;
    threads[slot].alarmAt = 0;
    threads[slot].pollActive = false;
    threads[slot].pollDeadline = 0;
    threads[slot].brk = 0;
    threads[slot].vruntime = 0;
    threads[slot].exitCode = 0;
    threads[slot].weight = schedDefaultWeight;
    threads[slot].everRan = false;
    threads[slot].waitChannel = nullptr;
    threads[slot].userSlotBase = elfSlotBase(entry);
    vfsFdInit(threads[slot].fds);
    sigResetState(&threads[slot]);

    return slot;
}

/* CFS-lite selection: every reschedule rebuilds a min-vruntime priority
   queue (prioq.h, backed by the intrusive rbt) from every currently
   Runnable thread and picks the minimum. The queue is deliberately
   ephemeral (built fresh each call, discarded after one pop) rather than a
   run-queue whose membership persists across calls -- a thread can become
   Runnable from code paths other than schedule() itself (schedWakeChannel,
   called from pipe.c on the producer/consumer side of a wait-queue), and
   rebuilding from the authoritative `state` field every time means there is
   never a second copy of "who is runnable" to fall out of sync. */
uint64 schedule(uint64 currentRsp)
{
    if (!schedulerActive)
        return currentRsp;

    Thread* current = &threads[currentIndex];
    current->rsp = currentRsp;
    if (current->state == threadRunning) {
        current->vruntime += schedTickCharge * schedDefaultWeight / current->weight;
        current->state = threadRunnable;
    }

    uint64 now = pitTicks();
    for (uint32 i = 0; i < threadTotal; i++)
        if (threads[i].state == threadSleeping && now >= threads[i].sleepUntil)
            threads[i].state = threadRunnable;

    /* sysPoll's own timeout, forced the exact same way sleepUntil's expiry
       is above: a thread parked on schedPollChannel because none of its
       requested fds were ready yet still needs to wake up on its own even
       if NONE of them ever becomes ready (pollDeadline == 0 means "wait
       forever", i.e. a real timeout of -1, and is deliberately never
       forced here). The retried sysPoll call (syscall.c) re-checks every
       fd from scratch on wake regardless of why it woke, so forcing
       Runnable here without clearing waitChannel first (same as the
       sleepUntil scan just above) is enough -- there is no separate
       "why did I wake" state to reconcile. */
    for (uint32 i = 0; i < threadTotal; i++)
        if (threads[i].state == threadBlocked && threads[i].pollActive &&
            threads[i].pollDeadline != 0 && now >= threads[i].pollDeadline)
            threads[i].state = threadRunnable;

    /* Real alarm(2) delivery: a deadline set by threadSetAlarm fires here,
       once per timer tick, regardless of what state its owning thread is
       currently in (Running, Sleeping, Blocked -- a real alarm keeps
       ticking no matter what the process is doing). Cleared BEFORE
       raising, not after, so one expiry fires sigAlrm exactly once, not
       once per tick until something else clears it; sigRaiseInfo (signal.h)
       already refuses a Dead/Unused target harmlessly, so a thread that
       exited with an alarm still pending needs no special case here.
       siCodeKernel/0/0: a real SIGALRM has no sending process and no exit
       status of its own to report (real Unix's own SIGALRM si_code is
       SI_KERNEL/SI_TIMER for exactly this reason -- it is the kernel's
       timer, not another process, that raised it). */
    for (uint32 i = 0; i < threadTotal; i++)
        if (threads[i].alarmAt != 0 && now >= threads[i].alarmAt) {
            threads[i].alarmAt = 0;
            sigRaiseInfo(threads[i].id, sigAlrm, siCodeKernel, 0, 0);
        }

    PrioQueue ready;
    prioqInit(&ready);
    for (uint32 i = 0; i < threadTotal; i++)
        if (threads[i].state == threadRunnable)
            prioqInsert(&ready, &threads[i].schedNode, threads[i].vruntime, threads[i].id);

    PrioQNode* winner = prioqPopMin(&ready);
    uint32 next = winner ? winner->id : currentIndex;

    currentIndex = next;
    threads[next].state = threadRunning;
    threads[next].everRan = true;
    tssSetRsp0(threads[next].kernelStackTop);
    return threads[next].rsp;
}

void schedStart(void)
{
    schedulerActive = true;
    enableInterrupts();
    for (;;)
        halt();
}

void threadExit(uint64 code)
{
    Thread* thread = &threads[currentIndex];
    thread->exitCode = code;
    thread->state = threadDead;
    kprintf("[user] pid=%u '%s' exited with code %lu\n", thread->id, thread->name, code);

    /* Real, asynchronous SIGCHLD (signal.h/signal.c) against whoever
       spawned this thread (Thread.parentId, sched.h) -- see threadExit's
       own doc comment for why this matters beyond threadJoin/sysWait's
       already-existing synchronous reap. sigRaiseInfo itself is what makes
       this safe against parentId 0 (idle) or an already-gone parent.
       siCodeChldExited/this thread's own id/code: real Unix's own
       WIFEXITED(status) + WEXITSTATUS(status), exposed directly as
       si_pid/si_status instead of a packed word a caller must decode. */
    sigRaiseInfo(thread->parentId, sigChld, siCodeChldExited, (int)thread->id, (int)code);

    /* Wake whichever thread (if any) is parked in threadJoin(thread->id) --
       the channel is this slot's own address, matching threadJoin's
       schedBlockCurrent(&threads[id]) below. Harmless no-op if nobody is
       joining this thread. */
    schedWakeChannel(thread);

    /* Also wake whichever thread (if any) is parked waiting for ANY child
       of thread->parentId (sysWait's waitAnyPid path, syscall.c, via
       schedBlockCurrent(currentThread())) -- see threadExit's own doc
       comment in sched.h. Channel is the PARENT's own slot address, not
       this dying thread's, so it is naturally distinct from the wake above
       and a harmless no-op if the parent never called userWaitAny. */
    schedWakeChannel(&threads[thread->parentId]);
}

bool threadKill(uint32 id)
{
    if (id == 0 || id >= maxThreads)
        return false;

    ThreadState state = threads[id].state;
    if (state != threadRunnable && state != threadSleeping &&
        state != threadBlocked && state != threadStopped)
        return false;

    threads[id].exitCode = threadKilledExitCode;
    threads[id].state = threadDead;
    kprintf("[sched] pid=%u '%s' killed\n", id, threads[id].name);

    /* Same sigChld-to-parent contract as threadExit -- a killed child is
       still a dead child as far as SIGCHLD is concerned (see threadKill's
       own doc comment). siCodeChldKilled/id/threadKilledExitCode: real
       Unix's own WIFSIGNALED(status) + WTERMSIG(status) would report the
       actual killing signal number; this kernel's threadKill is not tied
       to any one specific signal (SIGKILL's sigRaiseInfo delegates
       straight here without ever reaching sigPendingInfo itself, and
       userKill calls this directly with no signal at all) -- si_status
       reports threadKilledExitCode instead, the exact same "killed" value
       userWait already surfaces elsewhere in this kernel, not a new
       fabricated number. */
    sigRaiseInfo(threads[id].parentId, sigChld, siCodeChldKilled, (int)id, (int)threadKilledExitCode);

    /* Same wake-the-joiner contract as threadExit -- a thread already
       parked in threadJoin(id) (or the retry-syscall path in sysWait,
       syscall.c) must not wait for its victim to "finish" some
       in-progress work first, since a killed thread never gets to. */
    schedWakeChannel(&threads[id]);

    /* Same any-child wake as threadExit -- see threadKill's own doc
       comment in sched.h. */
    schedWakeChannel(&threads[threads[id].parentId]);
    return true;
}

uint32 threadPgid(uint32 id)
{
    if (id >= maxThreads || threads[id].state == threadUnused)
        return threadInvalidId;
    return threads[id].pgid;
}

bool threadSetPgid(uint32 id, uint32 pgid)
{
    if (id == 0 || id >= maxThreads)
        return false;
    ThreadState state = threads[id].state;
    if (state == threadDead || state == threadUnused)
        return false;

    threads[id].pgid = pgid;
    return true;
}

bool threadKillGroup(uint32 pgid)
{
    bool anyKilled = false;
    for (uint32 id = 1; id < threadTotal; id++) {
        if (threads[id].state == threadDead || threads[id].state == threadUnused)
            continue;
        if (threads[id].pgid != pgid)
            continue;
        if (threadKill(id))
            anyKilled = true;
    }
    return anyKilled;
}

const char* currentThreadCwd(void)
{
    return threads[currentIndex].cwd;
}

bool currentThreadSetCwd(const char* path)
{
    uint64 len = strlen(path);
    if (len >= fsPathMax)
        return false;
    memcpy(threads[currentIndex].cwd, path, len);
    threads[currentIndex].cwd[len] = 0;
    return true;
}

uint32 currentThreadUid(void)
{
    return threads[currentIndex].uid;
}

bool currentThreadSetUid(uint32 newUid)
{
    uint32 uid = threads[currentIndex].uid;
    if (uid != 0 && newUid != uid)
        return false;
    threads[currentIndex].uid = newUid;
    return true;
}

uint32 currentThreadGid(void)
{
    return threads[currentIndex].gid;
}

bool currentThreadSetGid(uint32 newGid)
{
    /* Privilege gate is this thread's own UID (root, CAP_SETGID-style), not
       its current GID -- see sched.h's own doc comment for why that
       mirrors real setgid(2) rather than currentThreadSetUid's shape. */
    if (threads[currentIndex].uid != 0) {
        uint32 gid = threads[currentIndex].gid;
        if (newGid != gid)
            return false;
    }
    threads[currentIndex].gid = newGid;
    return true;
}

/* True if `entry` (a stored "NAME=VALUE" slot) names the same variable as
   `name` -- i.e. `entry` starts with `name` followed immediately by '='.
   Comparing this way (rather than strcmp against a rebuilt "NAME=" prefix)
   needs no scratch buffer and stops at the first mismatching byte, same
   spirit as string.h's own strncmp. */
static bool envNameMatches(const char* entry, const char* name)
{
    while (*name) {
        if (*entry != *name)
            return false;
        entry++;
        name++;
    }
    return *entry == '=';
}

const char* currentThreadGetEnv(const char* name)
{
    Thread* self = &threads[currentIndex];
    for (int i = 0; i < self->envCount; i++) {
        if (envNameMatches(self->env[i], name)) {
            const char* value = self->env[i];
            while (*value != '=')
                value++;
            return value + 1;
        }
    }
}

void threadSleep(uint64 milliseconds)
{
    uint64 ticks = milliseconds * pitFrequency() / 1000;
    if (ticks == 0)
        ticks = 1;
    threads[currentIndex].sleepUntil = pitTicks() + ticks;
    threads[currentIndex].state = threadSleeping;
}

void threadSetBrk(uint32 id, uint64 brk)
{
    threads[id].brk = brk;
}

uint64 currentThreadSbrk(int64 increment)
{
    Thread* thread = &threads[currentIndex];
    uint64 previous = thread->brk;

    if (increment > 0)
        pagingMapUserRange(previous, (uint64)increment);

    thread->brk = previous + (uint64)increment;
    return previous;
}

uint32 threadCount(void)
{
    return threadTotal;
}

Thread* threadAt(uint32 index)
{
    return &threads[index];
}

Thread* currentThread(void)
{
    return &threads[currentIndex];
}

uint32 currentThreadId(void)
{
    return threads[currentIndex].id;
}
