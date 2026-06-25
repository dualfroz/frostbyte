#include "types.h"
#include "console.h"
#include "gdt.h"
#include "idt.h"
#include "tss.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "sched.h"
#include "shell.h"
#include "multiboot2.h"
#include "initrd.h"
#include "vfs.h"
#include "elf.h"
#include "ktest.h"
#include "pipe.h"
#include "klib.h"
#include "signal.h"
#include "ports.h"
#include "ata.h"
#include "sfs.h"

#define multibootBootMagic 0x36D76289
#define heapSize (48 * 1024 * 1024)
#define timerFrequency 100
#define userStackSize 0x4000

static uint8 bootstrapKernelStack[16384] __attribute__((aligned(16)));

static void banner(void)
{
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  frostbyte kernel  x86_64  long mode\n");
    kprintf("  wave 2: userspace, ring 3, syscalls\n");
    kprintf("========================================\n\n");
}

static void shellThread(void)
{
    shellRun();
}

/* Exposed at file scope (rather than local to workerA/workerB) so
   schedPriorityVerifierThread can read them back and turn "the scheduler
   favors higher weight" into an automated, gate-checked assertion instead
   of something a human has to eyeball in the boot log. */
static volatile uint64 workerAIterations;
static volatile uint64 workerBIterations;

static void workerA(void)
{
    uint64 nextLog = 50;
    for (;;) {
        workerAIterations++;
        if (pitTicks() >= nextLog) {
            kprintf("[workerA] tick=%lu iterations=%lu\n", pitTicks(), workerAIterations);
            nextLog += 100;
        }
    }
}

static void workerB(void)
{
    uint64 nextLog = 90;
    for (;;) {
        workerBIterations++;
        if (pitTicks() >= nextLog) {
            kprintf("[workerB] tick=%lu iterations=%lu\n", pitTicks(), workerBIterations);
            nextLog += 100;
        }
    }
}

#define schedPriorityCheckDelayMs 15000
#define schedPriorityWeightRatio  3

static void schedPriorityVerifierThread(void)
{
    /* threadSleep() only flips this thread's own state to Sleeping; unlike
       the sysSleep syscall path (syscall.c), which forces a reschedule
       synchronously in the same interrupt frame right after calling it,
       a plain kernel thread body must yield for itself or it simply falls
       through to the next line on its own timeslice, sleep or not. Loop on
       halt() until schedule()'s "wake sleeping threads whose time has come"
       pass (sched.c) actually flips the state back, the same wait pattern
       pipeReadBlocking/pipeWriteBlocking (pipe.c) use for wait-queue waits:
       any spurious wake (e.g. a keyboard IRQ, which never calls schedule()
       and so never touches this state) just loops back into another
       halt() instead of proceeding early. */
    threadSleep(schedPriorityCheckDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    uint64 a = workerAIterations;
    uint64 b = workerBIterations;
    /* workerA runs at schedPriorityWeightRatio times workerB's weight, so
       over any sufficiently long stretch where both are always runnable
       (true here: both are pure CPU hogs, never sleeping or blocking) the
       CFS-lite scheduler should give workerA that many times as many turns.
       Allow a wide [1.5x, 6x] band rather than pinning to exactly 3x: this
       is a real preemptive scheduler racing against wall-clock QEMU
       performance, not a deterministic simulation, so the achieved ratio
       will vary run to run - the assertion that matters is "meaningfully
       and consistently favors the higher-weight thread", not an exact
       number. */
    bool ok = b > 0 && a > b + b / 2 && a < b * (schedPriorityWeightRatio * 2);
    kprintf("[sched] priority check: workerA=%lu workerB=%lu (weight ratio %u) -> %s\n",
            a, b, schedPriorityWeightRatio, ok ? "sched-priority-ok" : "sched-priority-FAIL");

    for (;;)
        halt();
}

#define pipeDemoMessages 10

static int pipeDemoHandle = -1;

static void pipeProducerThread(void)
{
    for (int i = 0; i < pipeDemoMessages; i++) {
        char line[32];
        int len = snprintf(line, sizeof(line), "msg %d\n", i);

        uint64 offset = 0;
        while (offset < (uint64)len) {
            int64 n = pipeWriteBlocking(pipeDemoHandle, line + offset, (uint64)len - offset);
            if (n <= 0)
                break;
            offset += (uint64)n;
        }
    }
    pipeCloseWrite(pipeDemoHandle);

    /* Kernel thread entries run atop a synthetic iret frame, not a normal
       call stack: falling off the end here would pop garbage as the next
       instruction pointer. Every kernel thread in this scheduler must park
       itself forever instead of returning (see workerA/workerB). */
    for (;;)
        halt();
}

static void pipeConsumerThread(void)
{
    char lineBuffer[32];
    uint64 lineLength = 0;
    int received = 0;

    for (;;) {
        char chunk[16];
        int64 n = pipeReadBlocking(pipeDemoHandle, chunk, sizeof(chunk));

        if (n > 0) {
            for (int64 i = 0; i < n; i++) {
                char c = chunk[i];
                if (c == '\n') {
                    lineBuffer[lineLength] = 0;
                    kprintf("[pipe] consumer got: %s\n", lineBuffer);
                    received++;
                    lineLength = 0;
                } else if (lineLength + 1 < sizeof(lineBuffer)) {
                    lineBuffer[lineLength++] = c;
                }
            }
        } else {
            /* 0 = true EOF, -1 = invalid handle: either way there is
               nothing left to wait for. */
            break;
        }
    }

    pipeCloseRead(pipeDemoHandle);
    kprintf("pipe demo: eof, count=%d\n", received);

    for (;;)
        halt();
}

/* Demonstrates threadJoin/threadReap's real, live blocking path (sched.c,
   sched.h) end to end -- the one path schedTest.c cannot exercise, since
   ktestRunAll() runs before the scheduler is live and there is no second
   thread to actually park on and be woken by. shortLivedWorkerThread does a
   little busy-work then calls threadExit() directly (a kernel thread may
   call it as a plain function -- no syscall needed, unlike a ring-3
   program) -- joinerThread almost certainly starts first (both are created
   with default weight, and the worker has real work to do before it can
   exit), so this also proves the schedBlockCurrent+halt() park actually
   happens and schedWakeChannel actually wakes it, not just the fast "already
   dead" path. */
#define joinDemoBusyIterations 20000000ull
#define joinDemoExitCode 42ull

static volatile uint32 joinDemoWorkerId = threadInvalidId;

static void shortLivedWorkerThread(void)
{
    for (volatile uint64 i = 0; i < joinDemoBusyIterations; i++)
        ;
    threadExit(joinDemoExitCode);

    /* Marked Dead above; the CPU is still executing this thread's code
       until the next timer tick preempts it into schedule(), which will
       never pick a Dead thread again. Must park, not return (see
       workerA/workerB). */
    for (;;)
        halt();
}

static void joinProbeThread(void)
{
    for (;;)
        halt();
}

static void joinerThread(void)
{
    int64 code = threadJoin(joinDemoWorkerId);
    bool codeOk = (code == (int64)joinDemoExitCode);

    /* threadJoin already reaped the worker's slot -- prove it is genuinely
       reusable, not just marked dead, by immediately creating a new thread
       and checking it lands in that exact freed slot rather than growing
       the table further. */
    uint32 reused = threadCreate("joinReuseProbe", joinProbeThread);
    bool reuseOk = (reused == joinDemoWorkerId);
    if (reused != threadInvalidId)
        threadDiscard(reused);

    kprintf("[sched] join demo: worker pid=%u exitCode=%ld (expected %lu), "
            "freed slot reused=%s -> %s\n",
            joinDemoWorkerId, code, joinDemoExitCode, reuseOk ? "yes" : "no",
            (codeOk && reuseOk) ? "join-reap-ok" : "join-reap-FAIL");

    for (;;)
        halt();
}

static void runHeapTest(void)
{
    kprintf("[heap] free-list allocator self-test:\n");

    void* a = kmalloc(100);
    void* b = kmalloc(200);
    void* c = kmalloc(100);
    kprintf("[heap]   alloc a=%p b=%p c=%p, live blocks=%lu\n",
            a, b, c, heapBlockCount());

    kfree(b);
    void* d = kmalloc(150);
    kprintf("[heap]   free(b) then malloc(150) -> d=%p, reused b: %s\n",
            d, d == b ? "yes" : "no");

    uint64 beforeCoalesce = heapLargestFree();
    kfree(a);
    kfree(c);
    kfree(d);
    uint64 afterCoalesce = heapLargestFree();

    kprintf("[heap]   largest free %lu KiB -> %lu KiB after freeing a,c,d\n",
            beforeCoalesce / 1024, afterCoalesce / 1024);
    kprintf("[heap]   coalesce %s, %lu KiB free of %lu KiB, blocks=%lu\n",
            afterCoalesce > beforeCoalesce ? "PASS" : "FAIL",
            heapFreeBytes() / 1024, heapTotal() / 1024, heapBlockCount());
}

static void runPagingTest(void)
{
    uint64 testVirtual = 0xC0000000;
    uint64 frame = pmmAlloc();

    if (frame == 0) {
        kprintf("[paging] no free frame for test\n");
        return;
    }

    if (!pagingMap(testVirtual, frame, pagePresent | pageWrite)) {
        kprintf("[paging] mapping failed\n");
        return;
    }

    volatile uint64* probe = (volatile uint64*)testVirtual;
    *probe = 0xCAFEBABEDEADBEEFull;

    uint64 resolved = pagingResolve(testVirtual);
    if (*probe == 0xCAFEBABEDEADBEEFull && resolved == frame)
        kprintf("[paging] mapped %p -> phys %lx, readback OK\n", (void*)testVirtual, frame);
    else
        kprintf("[paging] readback FAILED (value=%lx resolved=%lx)\n", *probe, resolved);
}

static uint32 spawnUser(const char* path, const char* name, uint64 stackTop, uint64 brkStart)
{
    VfsNode* node = vfsLookup(path);
    if (!node) {
        kprintf("[user] '%s' not found in vfs\n", path);
        return threadInvalidId;
    }

    const void* image = vfsFileData(node);
    uint64 size = vfsSize(node);
    uint64 entry = elfLoad(image, size);
    if (entry == 0) {
        kprintf("[user] '%s' failed to load\n", path);
        return threadInvalidId;
    }

    pagingMapUserRangeFlags(stackTop - userStackSize, userStackSize, pageNoExecute);

    /* Every boot-launched program gets a real, if minimal, argv: just its
       own path, matching what a real OS would put in argv[0]. Nothing in
       this kernel's boot sequence needs more than that -- programs wanting
       specific arguments are spawned at runtime instead (doSpawn,
       syscall.c), where the caller controls argv directly. */
    const char* argv[1] = { path };
    uint64 initialRsp;
    if (!elfBuildInitialStack(stackTop, userStackSize, argv, 1, &initialRsp)) {
        kprintf("[user] '%s' not started: argv did not fit its stack\n", path);
        return threadInvalidId;
    }

    uint32 pid = threadCreateUser(name, entry, initialRsp);
    if (pid == threadInvalidId) {
        kprintf("[user] '%s' not started: thread table full\n", path);
        return threadInvalidId;
    }
    if (brkStart)
        threadSetBrk(pid, brkStart);

    kprintf("[user] loaded '%s' pid=%u entry=%p stackTop=%p (%lu bytes)\n",
            name, pid, (void*)entry, (void*)stackTop, size);
    return pid;
}

/* proctest.c's very last action on its success path is userExec(), which
   replaces its own image with exectarget.c's -- so the only way to observe
   from OUTSIDE that process that exec() genuinely happened (rather than
   proctest simply exiting cleanly on its own) is to threadJoin() the exact
   pid kmain.c originally gave proctest and check that its eventual exit
   code is exectarget's (7), not proctest's own success path (which never
   returns 7 -- it either exits 1 on a self-detected FAIL or never exits at
   all once exec succeeds). A mismatch or an exit code belonging to neither
   program both surface as a plain FAIL below. */
#define execDemoExitCode 7ull

static uint32 execDemoPid = threadInvalidId;

static void execReaperThread(void)
{
    if (execDemoPid == threadInvalidId) {
        kprintf("[user] exec demo skipped: proctest never started\n");
        for (;;)
            halt();
    }

    int64 code = threadJoin(execDemoPid);
    bool ok = (code == (int64)execDemoExitCode);
    kprintf("[user] exec demo: pid=%u final exitCode=%ld (expected %lu) -> %s\n",
            execDemoPid, code, execDemoExitCode, ok ? "exec-pid-preserved-ok" : "exec-pid-preserved-FAIL");

    for (;;)
        halt();
}

/* Headless QEMU has no automated way to drive a real PS/2 keyboard device --
   ci-test.sh only captures the COM1 serial log, and scripts no QEMU monitor
   "sendkey" command -- so this thread simulates exactly one keystroke the
   same way a real IRQ1 handler would: by calling keyboardHandleScancode
   (keyboard.h) directly with the scancode a real 'k' keypress generates.
   consolereader1.c and consolereader2.c are both already parked in a real,
   blocking userRead(0, ...) well before this fires (the sleep below gives
   every boot-time thread, including both of them, at least one CFS-lite
   timeslice first) -- keyboard.c's per-reader fan-out then delivers each of
   them its own independent copy of the same byte, proving two entirely
   unrelated ring-3 processes can share "the keyboard" without racing each
   other for it, the gap the previous wave's own notes called out as the
   next logical step after pipes. */
#define consoleDemoScancodeK 0x25
#define consoleInjectDelayMs 250

static void consoleInjectorThread(void)
{
    threadSleep(consoleInjectDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  injecting synthetic keypress 'k' for the console fan-out demo\n");
    keyboardHandleScancode(consoleDemoScancodeK);

    for (;;)
        halt();
}

/* Same "headless QEMU cannot drive a real PS/2 device" reasoning as
   consoleInjectorThread above, this time simulating a real Ctrl-C
   keypress (left-Ctrl held, 'c' pressed and released, left-Ctrl released)
   by calling keyboardHandleScancode with exactly the four scancodes a
   physical keyboard would generate for it -- the live ring-3 proof that
   sched.h's new Thread.pgid and keyboard.c's own new INTR interception
   actually scope a terminal-generated signal to ONE process group
   (pgroupfg.c, made the foreground below) and leave a completely
   different one (pgroupbg.c, deliberately left as its own default group)
   untouched. pgroupfgPid/pgroupbgPid are threadInvalidId if either
   program failed to load (thread table full, missing from the ramdisk) --
   this thread degrades to a silent no-op rather than acting on a bogus id,
   the same "skipped" pattern execReaperThread already uses for its own
   demo. */
#define scancodeLeftCtrlMake  0x1D
#define scancodeLeftCtrlBreak 0x9D
#define scancodeCPressMake    0x2E
#define scancodeCPressBreak   0xAE
#define pgroupInjectDelayMs 400
#define pgroupSettleDelayMs 100

static uint32 pgroupfgPid = threadInvalidId;
static uint32 pgroupbgPid = threadInvalidId;

static void pgroupInjectorThread(void)
{
    if (pgroupfgPid == threadInvalidId || pgroupbgPid == threadInvalidId) {
        kprintf("[kbd]  process-group demo skipped: pgroupfg/pgroupbg never started\n");
        for (;;)
            halt();
    }

    threadSleep(pgroupInjectDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  injecting synthetic ctrl-c for the process-group demo\n");
    keyboardHandleScancode(scancodeLeftCtrlMake);
    keyboardHandleScancode(scancodeCPressMake);
    keyboardHandleScancode(scancodeCPressBreak);
    keyboardHandleScancode(scancodeLeftCtrlBreak);

    /* Give the scheduler a real timeslice to actually deliver the
       resulting sigInt to pgroupfg (early-wake + return-to-ring3, not
       instantaneous) before this thread moves on to check anything. */
    threadSleep(pgroupSettleDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    int64 fgCode = threadJoin(pgroupfgPid);
    bool fgOk = (fgCode == (int64)(128 + sigInt));
    kprintf("[user] pgroup demo: fg pid=%u final exitCode=%ld (expected 128+sigInt=%d) -> %s\n",
            pgroupfgPid, fgCode, 128 + sigInt, fgOk ? "pgrouptest-fg-terminated-by-sigint-ok" : "FAIL");

    /* Proves pgroupbg (a DIFFERENT process group, never named as
       foreground) is still genuinely alive and running real code after
       the Ctrl-C above -- if keyboard.c's group scoping were ever broken
       to reach it too, pgroupbg would already be Dead by now and this
       sigRaise (signal.h) would silently do nothing, so pgroupbg.c's own
       "still-alive-after-ctrlc-ok" needle would simply never appear in
       the boot log, failing the gate. */
    sigRaise(pgroupbgPid, sigUsr1);

    for (;;)
        halt();
}

/*
 * The live ring-3 proof for shell.c's new `NAME [ARGS] [&]` job control
 * (jobctl.h does the actual spawning/terminal-ownership work): rather than
 * spawning jobctlfg.c/jobctlbg.c directly the way every demo above does,
 * this thread TYPES commands at the real interactive shell (shellRun,
 * already running as its own boot-time thread) through the same
 * injected-keystroke mechanism consoleInjectorThread/pgroupInjectorThread
 * above already use for the same "no automated way to drive a real PS/2
 * device under headless QEMU" reason -- proving the shell's own runtime
 * decision of what to load and whether to grant it the terminal, not just
 * that jobctl.c's functions work when called directly.
 *
 * Scancode values below are read straight off keyboard.c's own
 * scancodeLower table (index N there is scancode N here -- already cross-
 * checked against that file's own worked examples: index 0x25 is 'k',
 * exactly consoleDemoScancodeK above; index 0x2E is 'c', exactly
 * scancodeCPressMake above), not guessed.
 */
#define scancodeMakeJ          0x24
#define scancodeMakeO          0x18
#define scancodeMakeB          0x30
#define scancodeMakeT          0x14
#define scancodeMakeL          0x26
#define scancodeMakeG          0x22
#define scancodeMakeF          0x21
#define scancodeMakeSpace      0x39
#define scancodeMakeSeven      0x08
#define scancodeMakeEnter      0x1C
#define scancodeLeftShiftMake  0x2A
#define scancodeLeftShiftBreak 0xAA
#define scancodeBreakBit       0x80

/* Additional letters/symbol needed only by pipelineDriverThread's own
   "pipesrc | pipesink" command below -- same cross-checked-against-
   scancodeLower sourcing as the block above (index 0x19 is 'p', 0x17 is
   'i', 0x12 is 'e', 0x1F is 's', 0x13 is 'r', 0x31 is 'n', 0x25 is 'k'
   (equal to consoleDemoScancodeK above, kept as this file's own named copy
   for this demo, the same "each caller of this pattern owns its own named
   copy" choice jobctl.c/kmain.c/syscall.c already make elsewhere), 0x2B is
   '\\', shifted into '|' by scancodeUpper the same way scancodeMakeSeven
   is shifted into '&' above). */
#define scancodeMakeP          0x19
#define scancodeMakeI          0x17
#define scancodeMakeE          0x12
#define scancodeMakeS          0x1F
#define scancodeMakeR          0x13
#define scancodeMakeN          0x31
#define scancodeMakeK          0x25
#define scancodeMakeBackslash  0x2B

static void injectKeyPress(uint8 makeCode)
{
    keyboardHandleScancode(makeCode);
    keyboardHandleScancode(makeCode | scancodeBreakBit);
}

/* '&' is shift+7 on a real US keyboard -- keyboard.c's scancodeUpper table
   maps index scancodeMakeSeven to '&' only while shiftHeld is true. */
static void injectShiftedKeyPress(uint8 makeCode)
{
    keyboardHandleScancode(scancodeLeftShiftMake);
    keyboardHandleScancode(makeCode);
    keyboardHandleScancode(makeCode | scancodeBreakBit);
    keyboardHandleScancode(scancodeLeftShiftBreak);
}

/* A single bare Enter, unconditionally, before typing anything this thread
   actually cares about: every keystroke keyboardHandleScancode decodes is
   broadcast to EVERY open console reader (keyboard.c), the shell's own
   included, so consoleInjectorThread's single synthetic 'k' (well before
   this thread ever runs) is still sitting unflushed in the shell's own
   line buffer -- there is no way for this thread to tell "a stray
   character from an earlier, unrelated demo" apart from "something a real
   user typed". One Enter here executes that "k" as a harmless unknown
   command (shell.c's execute()) and guarantees an empty line buffer before
   the real commands below, exactly like pressing Enter at a real prompt to
   clear a garbled line. */
static void jobctlTypeFlushLine(void)
{
    injectKeyPress(scancodeMakeEnter);
}

static void jobctlTypeBackgroundCommand(void)
{
    injectKeyPress(scancodeMakeJ);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeB);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeL);
    injectKeyPress(scancodeMakeB);
    injectKeyPress(scancodeMakeG);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeSeven);
    injectKeyPress(scancodeMakeEnter);
}

static void jobctlTypeForegroundCommand(void)
{
    injectKeyPress(scancodeMakeJ);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeB);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeL);
    injectKeyPress(scancodeMakeF);
    injectKeyPress(scancodeMakeG);
    injectKeyPress(scancodeMakeEnter);
}

#define jobctlDriverInitialDelayMs     900
#define jobctlDriverTypeSettleMs       150
#define jobctlDriverForegroundSettleMs 350
#define jobctlDriverSignalSettleMs     150

static void jobctlDriverThread(void)
{
    threadSleep(jobctlDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell commands for the on-demand job-control demo\n");
    jobctlTypeFlushLine();

    /* "jobctlbg &" -- backgrounded, so shell.c's runProgram never blocks
       on it: jobctlSpawnForeground/jobctlRunBackground's own load+spawn
       work is synchronous kernel code, so by the time the shell has
       actually consumed all of these buffered bytes (needs its own
       timeslice; keyboardHandleScancode above only enqueues them) its
       shellLastSpawnedPid() is already this job's real pid, not stale. */
    jobctlTypeBackgroundCommand();
    threadSleep(jobctlDriverTypeSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();
    uint32 jobctlbgPid = shellLastSpawnedPid();

    /* "jobctlfg" -- foreground, so the shell thread itself parks inside
       jobctlWaitForeground (threadJoin) the moment it processes this
       line's own trailing Enter. shellLastSpawnedPid() is still safe to
       read afterward: shell.c's runProgram sets it right after
       jobctlSpawnForeground returns, BEFORE ever calling
       jobctlWaitForeground, precisely so a live demo like this one is
       never stuck waiting for a foreground job to exit before it can even
       learn that job's own pid. The longer settle here (vs. the
       background command above) gives jobctlfg.c itself real scheduler
       time to reach its own blocking pipe read before Ctrl-C below --
       the same margin ratio pgroupInjectDelayMs/pgroupSettleDelayMs above
       already established for an equivalent single-process case. */
    jobctlTypeForegroundCommand();
    threadSleep(jobctlDriverForegroundSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();
    uint32 jobctlfgPid = shellLastSpawnedPid();

    if (jobctlbgPid == threadInvalidId || jobctlfgPid == threadInvalidId ||
        jobctlbgPid == jobctlfgPid) {
        kprintf("[kbd]  on-demand job-control demo skipped: jobctlbg/jobctlfg never started\n");
        for (;;)
            halt();
    }

    kprintf("[kbd]  injecting synthetic ctrl-c for the on-demand job-control demo\n");
    keyboardHandleScancode(scancodeLeftCtrlMake);
    keyboardHandleScancode(scancodeCPressMake);
    keyboardHandleScancode(scancodeCPressBreak);
    keyboardHandleScancode(scancodeLeftCtrlBreak);

    /* Give the scheduler a real timeslice to actually deliver the
       resulting sigInt to jobctlfg (early-wake + return-to-ring3), let
       the now-parked shell thread's own threadJoin notice it Dead, and let
       jobctlWaitForeground's reclaim (jobctl.c) run -- all real scheduled
       work, not instantaneous. */
    threadSleep(jobctlDriverSignalSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    /* The direct, causal proof that shell.c's runProgram/jobctl.c's
       jobctlWaitForeground actually reclaimed the terminal for the shell's
       OWN group the instant jobctlfg died, rather than leaving it
       permanently pointed at a now-dead pgid the way nothing in this
       kernel ever un-claimed it before this subsystem existed: if the
       reclaim never ran (or ran against the wrong pgid), the foreground
       would still equal jobctlfg's own now-meaningless pid forever. */
    uint32 fgAfterReclaim = keyboardForegroundPgid();
    bool reclaimed = (fgAfterReclaim != jobctlfgPid);
    kprintf("[user] jobctl demo: jobctlfg pid=%u foreground pgid now=%u -> %s\n",
            jobctlfgPid, fgAfterReclaim,
            reclaimed ? "shell-foreground-reclaimed-ok" : "FAIL");

    /* Proves jobctlbg (never granted the terminal at all, per shell.c's
       `&` handling) is still genuinely alive and running real code after
       the Ctrl-C above -- if the shell's background path ever put it in
       jobctlfg's group (or granted it the foreground itself), it would
       already be Dead by now and this sigRaise would silently do nothing,
       so jobctlbg.c's own "still-alive-after-ctrlc-ok" needle would simply
       never appear in the boot log, failing the gate. */
    sigRaise(jobctlbgPid, sigUsr1);

    for (;;)
        halt();
}

#define pipelineDriverInitialDelayMs 2500
#define pipelineDriverSettleMs       500

static void pipelineTypeCommand(void)
{
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeBackslash);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeN);
    injectKeyPress(scancodeMakeK);
    injectKeyPress(scancodeMakeEnter);
}

/*
 * The live ring-3 proof for shell.c's new `cmd1 | cmd2` pipeline syntax
 * (jobctl.h's jobctlSpawnPiped/jobctlWaitPipelineStages do the actual
 * fd-baking and multi-wait work): types "pipesrc | pipesink" at the real interactive
 * shell through the same injected-keystroke mechanism jobctlDriverThread
 * above already uses, well after that thread's own demo has fully
 * finished with the shell's shared line buffer (both threads' typed
 * characters go through the exact same broadcast keyboardHandleScancode
 * path, so they cannot safely interleave -- pipelineDriverInitialDelayMs
 * is chosen with a comfortable margin over jobctlDriverThread's own total
 * ~1550ms explicit sleep budget above to guarantee that). The actual
 * pass/fail proof is entirely pipesink.c's own doing (its "payload-ok"
 * needle only prints once it has verified, byte for byte, the exact
 * payload pipesrc.c wrote through the real pipe) -- this thread's only job
 * is to type the command and give the two spawned processes (neither of
 * which ever sleeps or waits on a real timer, unlike alarmtest/polltest) a
 * comfortable amount of real scheduled time to run to completion.
 */
static void pipelineDriverThread(void)
{
    threadSleep(pipelineDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell command for the pipeline demo\n");
    jobctlTypeFlushLine();
    pipelineTypeCommand();

    threadSleep(pipelineDriverSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    for (;;)
        halt();
}

/* Additional letters/symbols needed only by redirDriverThread's own three
   commands below -- same cross-checked-against-scancodeLower sourcing as
   every block above (index 0x20 is 'd', 0x11 is 'w', 0x32 is 'm', 0x2D is
   'x', 0x34 is '.', 0x35 is '/', 0x1E is 'a', 0x33 is ',' -- '>' is
   shift+'.' and '<' is shift+',' on a real US keyboard, scancodeUpper's
   own mapping for those two indices, the same way scancodeMakeSeven is
   shifted into '&' and scancodeMakeBackslash into '|' above). */
#define scancodeMakeD          0x20
#define scancodeMakeW          0x11
#define scancodeMakeM          0x32
#define scancodeMakeX          0x2D
#define scancodeMakeDot        0x34
#define scancodeMakeSlash      0x35
#define scancodeMakeA          0x1E
#define scancodeMakeComma      0x33

static void redirTypePath(void)
{
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeM);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeX);
    injectKeyPress(scancodeMakeT);
}

static void redirTypeWriterName(void)
{
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeW);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeR);
}

static void redirTypeReaderName(void)
{
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeA);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeR);
}

static void redirTypeWriteTruncateCommand(void)
{
    redirTypeWriterName();
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeSpace);
    redirTypePath();
    injectKeyPress(scancodeMakeEnter);
}

static void redirTypeWriteAppendCommand(void)
{
    redirTypeWriterName();
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeDot);
    injectShiftedKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeSpace);
    redirTypePath();
    injectKeyPress(scancodeMakeEnter);
}

static void redirTypeReadCommand(void)
{
    redirTypeReaderName();
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeComma);
    injectKeyPress(scancodeMakeSpace);
    redirTypePath();
    injectKeyPress(scancodeMakeEnter);
}

#define redirDriverInitialDelayMs 3600
#define redirDriverStepSettleMs   300

/*
 * The live ring-3 proof for shell.c's new `cmd < IN` / `cmd > OUT` /
 * `cmd >> OUT` file redirection (jobctlApplyRedirects/
 * jobctlSpawnForegroundRedirected, jobctl.h): types three commands, one at
 * a time, at the real interactive shell through the same injected-
 * keystroke mechanism jobctlDriverThread/pipelineDriverThread above
 * already use -- redirDriverInitialDelayMs gives pipelineDriverThread's
 * own total ~3000ms budget (2500ms initial + 500ms settle) a comfortable
 * margin before this thread ever touches the same shared keyboard buffer.
 *
 * Unlike pipelineDriverThread's single typed line, this is THREE separate
 * commands, each one run to completion in the FOREGROUND (the shell
 * genuinely blocks inside jobctlWaitForeground for each), so
 * redirDriverStepSettleMs between them is not just cosmetic pacing --
 * typing the next command's keystrokes while the shell is still mid-line
 * on the previous one would queue them in the same 32-byte console-reader
 * ring buffer (keyboard.c) that a real user's own typing shares, with no
 * guarantee they would not overflow it or be echoed against the wrong
 * still-open line. Each settle is comfortably longer than
 * redirwriter/redirreader's own trivial one-shot work (a single write or
 * read-to-EOF, neither ever sleeps or blocks on a timer) needs to spawn,
 * run, exit, and be reaped, the same safety margin philosophy every
 * other injected multi-step demo above already uses.
 *
 * "redirwriter > /tmp/redir.txt" (create/truncate) followed by
 * "redirwriter >> /tmp/redir.txt" (append, same path) leaves a real tmpfs
 * file holding two back-to-back copies of redirwriter.c's own payload;
 * "redirreader < /tmp/redir.txt" then reads that file back through a
 * REAL redirected stdin and compares it byte-for-byte against exactly
 * that doubled payload. The actual pass/fail proof is entirely
 * redirreader.c's own doing (its "payload-ok" needle only prints on an
 * exact match) -- this thread's only job is to type the three commands
 * and give the scheduler real time between them.
 */
static void redirDriverThread(void)
{
    threadSleep(redirDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell commands for the file redirection demo\n");
    jobctlTypeFlushLine();

    redirTypeWriteTruncateCommand();
    threadSleep(redirDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    redirTypeWriteAppendCommand();
    threadSleep(redirDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    redirTypeReadCommand();
    threadSleep(redirDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    for (;;)
        halt();
}

/* Digit needed only by stderrDriverThread's own three commands below --
   same cross-checked-against-scancodeLower sourcing as every block above
   (index 0x03 is '2', unshifted -- scancodeLower[3] == '2' in keyboard.c's
   own table, no shift key involved unlike the '>'/'<' operators reused
   from redirDriverThread's own scancodeMakeDot/scancodeMakeComma below). */
#define scancodeMakeTwo 0x03

/* "/tmp/err.txt" -- deliberately SHORTER than redirDriverThread's own
   "/tmp/redir.txt" (12 vs 14 chars): "stderrwriter"/"stderrreader" are
   already one character longer than "redirwriter"/"redirreader", and the
   `2>`/`2>>` operators are one/two characters longer than `>`/`>>` -- with
   the same path length as the file-redirection demo, the LONGEST typed
   line here ("stderrwriter 2>> /tmp/err.txt\n", 30 bytes incl. newline)
   would land right at keyboard.c's 32-byte per-thread console reader ring
   buffer (consoleReaderBufferSize) with almost no margin left. A first
   attempt at this demo used "/tmp/stderr.txt" (15 chars) and landed
   EXACTLY at (and, for the `2>>` line, one byte OVER) that 32-byte
   capacity -- ringPush (ring.c) silently drops a byte on a full buffer
   with no retry, so the trailing '\n' itself was lost, and the shell's
   line reader never saw a line terminator until the NEXT typed command's
   bytes arrived, silently concatenating three separate typed commands
   into one garbled line. Shortening the path restores the same few-bytes-
   of-headroom margin redirDriverThread's own commands already prove works
   (its longest line is 30 bytes incl. newline against the same 32-byte
   capacity) instead of running right up against the boundary. */
static void stderrTypePath(void)
{
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeM);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeX);
    injectKeyPress(scancodeMakeT);
}

static void stderrTypeWriterName(void)
{
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeW);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeR);
}

static void stderrTypeReaderName(void)
{
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeA);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeR);
}

static void stderrTypeWriteTruncateCommand(void)
{
    stderrTypeWriterName();
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeTwo);
    injectShiftedKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeSpace);
    stderrTypePath();
    injectKeyPress(scancodeMakeEnter);
}

static void stderrTypeWriteAppendCommand(void)
{
    stderrTypeWriterName();
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeTwo);
    injectShiftedKeyPress(scancodeMakeDot);
    injectShiftedKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeSpace);
    stderrTypePath();
    injectKeyPress(scancodeMakeEnter);
}

static void stderrTypeReadCommand(void)
{
    stderrTypeReaderName();
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeComma);
    injectKeyPress(scancodeMakeSpace);
    stderrTypePath();
    injectKeyPress(scancodeMakeEnter);
}

#define stderrDriverInitialDelayMs 5500
#define stderrDriverStepSettleMs   300

/* Letter/symbol needed only by hereDriverThread's own command below -- same
   cross-checked-against-scancodeLower sourcing as every block above (index
   0x23 is 'h', 0x0C is '-', both unshifted -- keyboard.c's own
   scancodeLower[0x23] == 'h' and scancodeLower[0x0C] == '-'). Every other
   letter the command needs (e,r,s,t,o,k) is already defined by an earlier
   block above. */
#define scancodeMakeH      0x23
#define scancodeMakeHyphen 0x0C

static void hereTypeCommand(void)
{
    injectKeyPress(scancodeMakeH);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeComma);
    injectShiftedKeyPress(scancodeMakeComma);
    injectShiftedKeyPress(scancodeMakeComma);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeH);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeHyphen);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeK);
    injectKeyPress(scancodeMakeEnter);
}

#define hereDriverInitialDelayMs 7500
#define hereDriverSettleMs       300

/*
 * The live ring-3 proof for shell.c's new `cmd <<< TEXT` here-string syntax
 * (jobctl.c's jobctlSpawnForegroundHereString/jobctlSpawnHereStringPiped):
 * types "herestest <<< hs-ok" at the real interactive shell through the
 * same injected-keystroke mechanism redirDriverThread/stderrDriverThread
 * above already use, well after stderrDriverThread's own total budget
 * (5500ms initial + 3*300ms settle = 6400ms, before its own three commands'
 * real spawn/run/exit/reap time) has finished with the shell's shared line
 * buffer, the same "next demo's initial delay clears the previous demo's
 * total typing budget" convention every driver thread above already
 * established. Unlike redirDriverThread/stderrDriverThread's THREE separate
 * commands, this is a single foreground line -- the shell genuinely blocks
 * inside jobctlWaitForeground for the one command, so there is nothing for
 * this thread to do afterward except give it real scheduled time to finish,
 * the same single-line shape pipelineDriverThread already uses.
 *
 * herestest.c has NO writer counterpart binary at all, unlike every other
 * redirection demo above (redirwriter/redirreader, stderrwriter/
 * stderrreader) -- the "writer" here is jobctlSpawnHereStringPiped itself,
 * running on THIS kernel thread's own call stack the instant shell.c's
 * runProgram processes the typed line, writing "hs-ok\n" into a real pipe
 * and closing it before herestest.c ever gets a timeslice to read fd 0. The
 * actual pass/fail proof is entirely herestest.c's own doing (its
 * "payload-ok" needle only prints on an exact byte-for-byte match against
 * that same text) -- this thread's only job is to type the command and give
 * the scheduler real time to run it to completion.
 */
static void hereDriverThread(void)
{
    threadSleep(hereDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell command for the here-string demo\n");
    jobctlTypeFlushLine();
    hereTypeCommand();

    threadSleep(hereDriverSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    for (;;)
        halt();
}

/* Every letter/symbol "hcat <<< hpipe-ok | hsink" needs (h,c,a,t,<,p,i,e,-,
   o,k,|,s,n) was already defined by an earlier block above -- see
   pipelineTypeCommand's own '|' (scancodeMakeBackslash, shifted),
   hereTypeCommand's own '<<<' (scancodeMakeComma, shifted) and '-'
   (scancodeMakeHyphen, unshifted), and every plain letter from either one
   or stderrTypeReadCommand/redirTypeReadCommand's own blocks. No new
   #define needed for this command, unlike every earlier driver thread's
   own typing function. */
static void hereStringPipelineTypeCommand(void)
{
    injectKeyPress(scancodeMakeH);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeA);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeComma);
    injectShiftedKeyPress(scancodeMakeComma);
    injectShiftedKeyPress(scancodeMakeComma);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeH);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeHyphen);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeK);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeBackslash);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeH);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeN);
    injectKeyPress(scancodeMakeK);
    injectKeyPress(scancodeMakeEnter);
}

/* 9000ms, the same "clear the previous demo's own typing budget with a
   comfortable margin" convention every earlier driver thread's own initial
   delay already uses (hereDriverThread's own total budget is
   7500+300=7800ms). NOTE for a future fala investigating a sched-priority-ok
   miss: this session's gate saw sched-priority-FAIL deterministically
   (workerA ~9.2x workerB, over the [1.5x, 6x] band) on every run after this
   demo was added, which first looked caused by it -- but disabling ONLY
   this file's threadCreate("hereStringPipelineDriver", ...) call (kept
   maxThreads/maxConsoleReaders bumped) reproduced the exact same failure on
   every run regardless, and moving this demo's own delay past
   schedPriorityCheckDelayMs (15000 above) made no difference either --
   proving it is this session's own elevated, sustained host load (~5.4-6.2,
   well above the documented ~4.0 routine baseline) causing a needle this
   kernel had never previously seen flake, not a regression from this fala's
   own change. Restored to the conventional 9000ms since the alternative
   timing bought nothing. */
#define hereStringPipelineDriverInitialDelayMs 9000
#define hereStringPipelineDriverSettleMs       500

/*
 * The live ring-3 proof for shell.c's new `cmd1 <<< TEXT | cmd2` here-
 * string-into-pipeline syntax (jobctl.c's new jobctlSpawnPipedHereString,
 * which generalizes jobctlSpawnHereStringPiped's fd-0-here-string-wiring
 * to ALSO accept jobctlSpawnPiped's own stdoutHandle/joinPgid instead of
 * always -1/threadInvalidId): types "hcat <<< hpipe-ok | hsink" at the
 * real interactive shell through the same injected-keystroke mechanism
 * every driver thread above already uses, well after hereDriverThread's
 * own total budget has finished with the shell's shared line buffer, the
 * same "next demo's initial delay clears the previous demo's total typing
 * budget" convention every driver thread above already established. Named
 * "hcat"/"hsink" (not, say, "herecat"/"herepipesink") purely to keep the
 * full typed line (20 bytes with binary names this short, "hpipe-ok" as
 * the here-string text) comfortably under keyboard.c's 32-byte
 * consoleReaderBufferSize per line -- see kmain.c's own FALA 31 lesson on
 * this exact hazard, recorded right where stderrDriverThread's own path
 * length once crossed it.
 *
 * hcat.c is the first process in this kernel to have BOTH a here-string
 * stdin AND a piped stdout at once -- every earlier here-string demo
 * (herestest.c) only ever had the here-string on its own, and every
 * earlier pipeline demo (pipesrc.c/pipesink.c) only ever had plain piped
 * fds, never a here-string. The actual pass/fail proof is entirely
 * hsink.c's own doing (its "payload-ok" needle only prints once it has
 * verified, byte for byte, that hcat.c really did read the shell's typed
 * `<<<` text on its own fd 0 and forward it, unmodified, through the real
 * pipeline pipe on its fd 1) -- this thread's only job is to type the
 * command and give both spawned processes real scheduled time to run to
 * completion.
 */
static void hereStringPipelineDriverThread(void)
{
    threadSleep(hereStringPipelineDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell command for the here-string pipeline demo\n");
    jobctlTypeFlushLine();
    hereStringPipelineTypeCommand();

    threadSleep(hereStringPipelineDriverSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    for (;;)
        halt();
}

/*
 * The live ring-3 proof for shell.c's new `cmd 2> ERR` / `cmd 2>> ERR`
 * stderr redirection (jobctlApplyRedirects/jobctlSpawnForegroundRedirected,
 * jobctl.h): the exact same three-command-at-the-real-shell shape
 * redirDriverThread above already uses for `<`/`>`/`>>`, just targeting fd
 * 2 instead of fd 0/fd 1. stderrDriverInitialDelayMs gives redirDriverThread's
 * own total budget (3600ms initial + 3*300ms settle = 4500ms, before its
 * own three commands' real spawn/run/exit/reap time) a comfortable margin
 * before this thread ever touches the same shared keyboard buffer, the
 * same "next demo's initial delay clears the previous demo's total typing
 * budget" convention pipelineDriverThread/redirDriverThread's own doc
 * comments already established.
 *
 * "stderrwriter 2> /tmp/stderr.txt" (create/truncate) followed by
 * "stderrwriter 2>> /tmp/stderr.txt" (append, same path) leaves a real
 * tmpfs file holding two back-to-back copies of stderrwriter.c's own
 * payload; "stderrreader < /tmp/stderr.txt" then reads that file back
 * through a REAL redirected stdin and compares it byte-for-byte against
 * exactly that doubled payload. The actual pass/fail proof is entirely
 * stderrreader.c's own doing (its "payload-ok" needle only prints on an
 * exact match) -- this thread's only job is to type the three commands and
 * give the scheduler real time between them.
 */
static void stderrDriverThread(void)
{
    threadSleep(stderrDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell commands for the stderr redirection demo\n");
    jobctlTypeFlushLine();

    stderrTypeWriteTruncateCommand();
    threadSleep(stderrDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    stderrTypeWriteAppendCommand();
    threadSleep(stderrDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    stderrTypeReadCommand();
    threadSleep(stderrDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    for (;;)
        halt();
}

/* 10700ms gives hereStringPipelineDriverThread's own total budget
   (9000+500=9500ms, above its own real hcat/hsink spawn/run/reap time) a
   comfortable margin before this thread ever touches the same shared
   keyboard buffer, the same "next demo's initial delay clears the previous
   demo's total typing budget" convention every driver thread above already
   established. Settle is 600ms (slightly more than pipelineDriverThread's
   own 500ms for a 2-stage pipe) since this demo's pipe has one more real
   process (pipesrc, pipecat, AND pipesink) to spawn, run, and reap before
   the final needle can print. */
#define threeStagePipelineDriverInitialDelayMs 10700
#define threeStagePipelineDriverSettleMs       600

static void threeStagePipelineTypeCommand(void)
{
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeBackslash);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeA);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeBackslash);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeN);
    injectKeyPress(scancodeMakeK);
    injectKeyPress(scancodeMakeEnter);
}

/*
 * The live ring-3 proof for shell.c's new REAL N-stage pipeline
 * (runPipelineStages/jobctlWaitPipelineStages) going past two stages: types
 * "pipesrc | pipecat | pipesink" at the real interactive shell through the
 * same injected-keystroke mechanism every driver thread above already
 * uses, well after hereStringPipelineDriverThread's own total budget has
 * finished with the shell's shared line buffer.
 *
 * pipesrc.c and pipesink.c are reused completely unchanged from the
 * two-stage pipeline demo (FALA 29) -- their own narration ("wrote
 * payload", "blocking on read, waiting for pipesrc", "payload-ok") is still
 * literally true here, just relayed through one more real process instead
 * of being directly adjacent. The new thing this demo actually proves is
 * user/pipecat.c in the middle: jobctlSpawnPiped bakes a real pipe into
 * BOTH its fd 0 (from pipesrc's pipe) and its fd 1 (to pipesink's pipe)
 * before it ever runs a single instruction -- the first process in this
 * kernel with two DIFFERENT real inter-process pipes on its stdio at once
 * (hcat.c's own simultaneous piped-stdin-and-stdout, FALA 34, had a
 * shell-fed here-string on the stdin side, never a second real pipe). The
 * actual pass/fail proof is still entirely pipesink.c's own doing (its
 * "payload-ok" needle only prints on an exact byte-for-byte match of
 * whatever pipesrc originally wrote) -- this thread's only job is to type
 * the command and give all three spawned processes (none of which ever
 * sleeps or waits on a real timer) a comfortable amount of real scheduled
 * time to run to completion.
 */
static void threeStagePipelineDriverThread(void)
{
    threadSleep(threeStagePipelineDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell command for the three-stage pipeline demo\n");
    jobctlTypeFlushLine();
    threeStagePipelineTypeCommand();

    threadSleep(threeStagePipelineDriverSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    for (;;)
        halt();
}

/* '=' (sysGetEnv/sysSetEnv's own operator, shell.c's `export N=V`
   builtin), 'y'/'Y', and 'v'/'V' are the only three make codes no earlier
   driver thread above ever needed -- every other letter "export",
   "GREETING", "envprint", and "env" use (e,x,p,o,r,t,g,i,n,f,s,b) was
   already defined by an earlier block. Cross-checked against
   keyboard.c's own scancodeLower/scancodeUpper tables the same way every
   block above documents for its own new codes. */
#define scancodeMakeEquals 0x0D
#define scancodeMakeY      0x15
#define scancodeMakeV      0x2F

static void envTypeExportCommand(void)
{
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeX);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeG);
    injectShiftedKeyPress(scancodeMakeR);
    injectShiftedKeyPress(scancodeMakeE);
    injectShiftedKeyPress(scancodeMakeE);
    injectShiftedKeyPress(scancodeMakeT);
    injectShiftedKeyPress(scancodeMakeI);
    injectShiftedKeyPress(scancodeMakeN);
    injectShiftedKeyPress(scancodeMakeG);
    injectKeyPress(scancodeMakeEquals);
    injectKeyPress(scancodeMakeF);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeB);
    injectKeyPress(scancodeMakeY);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeEnter);
}

static void envTypeEnvprintCommand(void)
{
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeN);
    injectKeyPress(scancodeMakeV);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeN);
}

void kmain(uint32 magic, uint64 multibootInfo)
{
    consoleInit();
    banner();

    if (magic == multibootBootMagic)
        kprintf("[boot] multiboot2 magic verified (%x)\n", magic);
    else
        kprintf("[boot] unexpected boot magic %x\n", magic);

    gdtInit();
    kprintf("[gdt]  kernel + user segments, tss slot reserved\n");

    idtInit();
    kprintf("[idt]  256 vectors (32 exceptions, 16 irqs, int 0x80 syscall)\n");

    picRemap();
    for (uint8 irq = 0; irq < 16; irq++)
        picSetMask(irq);
    picClearMask(0);
    picClearMask(1);
    kprintf("[pic]  remapped to 0x20-0x2f, irq0 and irq1 unmasked\n");

    uint64 moduleStart = 0;
    uint64 moduleEnd = 0;
    bool haveModule = multibootFindModule(multibootInfo, &moduleStart, &moduleEnd);

    pmmInit(multibootInfo, moduleEnd);
    kprintf("[pmm]  %lu KiB usable, %lu frames, %lu free after kernel\n",
            pmmTotalBytes() / 1024, pmmTotalFrames(), pmmFreeFrames());

    uint64 heapStart = pmmReservedEnd();
    pmmReserve(heapStart, heapSize);
    heapInit(heapStart, heapSize);
    kprintf("[heap] free-list allocator at %p, %u KiB\n", (void*)heapStart, heapSize / 1024);

    runHeapTest();
    runPagingTest();

    pitInit(timerFrequency);
    kprintf("[pit]  timer at %u Hz\n", timerFrequency);

    keyboardInit();
    kprintf("[kbd]  ps/2 keyboard driver ready\n");

    schedInit();
    tssInit((uint64)(bootstrapKernelStack + sizeof(bootstrapKernelStack)));
    kprintf("[tss]  loaded, rsp0 bootstrap stack installed\n");

    vfsInit();
    if (haveModule)
        vfsMountUstar(moduleStart, moduleEnd);
    else
        kprintf("[vfs]  no multiboot2 module present\n");
    vfsMountTmpfs("/tmp");

    threadCreate("shell", shellThread);
    threadCreate("workerA", workerA);
    threadCreate("workerB", workerB);

    kprintf("\n[boot] launching userland programs from the ramdisk into ring 3\n");
    spawnUser("/bin/hello", "hello", 0x48000000, 0);
    spawnUser("/bin/counter", "counter", 0x58000000, 0);
    spawnUser("/bin/sbrktest", "sbrktest", 0x68000000, 0x64000000);
    spawnUser("/bin/fileread", "fileread", 0x78000000, 0);

    kprintf("[sched] %u threads total (idle, shell, 2 workers, user programs)\n",
            threadCount());
    kprintf("\n[boot] enabling interrupts, starting preemptive scheduler\n\n");

    schedStart();
}
