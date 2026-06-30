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
#define threadStackSize 16384

static Thread threads[maxThreads];
static uint32 threadTotal;
static uint32 currentIndex;
static bool schedulerActive;

void schedInit(void)
{
    threadTotal = 1;
    currentIndex = 0;

    threads[0].id = 0;
    threads[0].name = "idle";
    threads[0].state = threadRunning;
    threads[0].rsp = 0;
    threads[0].stackBase = 0;
    threads[0].kernelStackTop = 0;
    threads[0].sleepUntil = 0;
    threads[0].brk = 0;
    vfsFdInit(threads[0].fds);
}

uint32 threadCreate(const char* name, ThreadEntry entry)
{
    uint32 slot = threadTotal;
    threadTotal++;

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
    threads[slot].name = name;
    threads[slot].state = threadRunnable;
    threads[slot].stackBase = stackBase;
    threads[slot].kernelStackTop = stackTop;
    threads[slot].rsp = (uint64)frame;
    threads[slot].sleepUntil = 0;
    threads[slot].brk = 0;
    vfsFdInit(threads[slot].fds);

    return slot;
}

uint32 threadCreateUser(const char* name, uint64 entry, uint64 userStackTop)
{
    uint32 slot = threadTotal;
    threadTotal++;

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
    threads[slot].name = name;
    threads[slot].state = threadRunnable;
    threads[slot].stackBase = stackBase;
    threads[slot].kernelStackTop = kernelStackTop;
    threads[slot].rsp = (uint64)frame;
    threads[slot].sleepUntil = 0;
    threads[slot].brk = 0;
    vfsFdInit(threads[slot].fds);

    return slot;
}

uint64 schedule(uint64 currentRsp)
{
    if (!schedulerActive)
        return currentRsp;

    threads[currentIndex].rsp = currentRsp;
    if (threads[currentIndex].state == threadRunning)
        threads[currentIndex].state = threadRunnable;

    uint64 now = pitTicks();
    for (uint32 i = 0; i < threadTotal; i++)
        if (threads[i].state == threadSleeping && now >= threads[i].sleepUntil)
            threads[i].state = threadRunnable;

    uint32 next = 0;
    for (uint32 step = 1; step <= threadTotal; step++) {
        uint32 candidate = (currentIndex + step) % threadTotal;
        if (threads[candidate].state == threadRunnable) {
            next = candidate;
            break;
        }
    }

    currentIndex = next;
    threads[next].state = threadRunning;
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
    thread->state = threadDead;
    kprintf("[user] pid=%u '%s' exited with code %lu\n", thread->id, thread->name, code);
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
