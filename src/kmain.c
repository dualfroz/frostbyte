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
#include "string.h"

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
    kprintf("  multiboot2 / ring 3 / vfs / signals\n");
    kprintf("========================================\n\n");
}

static void shellThread(void)
{
    shellRun();
}

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
    threadSleep(schedPriorityCheckDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    uint64 a = workerAIterations;
    uint64 b = workerBIterations;
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
            break;
        }
    }

    pipeCloseRead(pipeDemoHandle);
    kprintf("pipe demo: eof, count=%d\n", received);

    for (;;)
        halt();
}

#define joinDemoBusyIterations 20000000ull
#define joinDemoExitCode 42ull

static volatile uint32 joinDemoWorkerId = threadInvalidId;
static uint32 joinDemoWorkerGeneration;

static void shortLivedWorkerThread(void)
{
    for (volatile uint64 i = 0; i < joinDemoBusyIterations; i++)
        ;
    threadExit(joinDemoExitCode);

    for (;;)
        halt();
}

static void joinerThread(void)
{
    int64 code = threadJoin(joinDemoWorkerId);
    bool codeOk = (code == (int64)joinDemoExitCode);

    const Thread* slot = threadAt(joinDemoWorkerId);
    bool freedOk = slot->state == threadUnused || slot->generation != joinDemoWorkerGeneration;

    kprintf("[sched] join demo: worker pid=%u exitCode=%ld (expected %lu), "
            "slot freed=%s -> %s\n",
            joinDemoWorkerId, code, joinDemoExitCode, freedOk ? "yes" : "no",
            (codeOk && freedOk) ? "join-reap-ok" : "join-reap-FAIL");

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

    threadSleep(pgroupSettleDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    int64 fgCode = threadJoin(pgroupfgPid);
    bool fgOk = (fgCode == (int64)(128 + sigInt));
    kprintf("[user] pgroup demo: fg pid=%u final exitCode=%ld (expected 128+sigInt=%d) -> %s\n",
            pgroupfgPid, fgCode, 128 + sigInt, fgOk ? "pgrouptest-fg-terminated-by-sigint-ok" : "FAIL");

    sigRaise(pgroupbgPid, sigUsr1);

    for (;;)
        halt();
}

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

static void injectShiftedKeyPress(uint8 makeCode)
{
    keyboardHandleScancode(scancodeLeftShiftMake);
    keyboardHandleScancode(makeCode);
    keyboardHandleScancode(makeCode | scancodeBreakBit);
    keyboardHandleScancode(scancodeLeftShiftBreak);
}

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

#define jobctlDriverInitialDelayMs 900
#define jobctlDriverPollMs         20

static void jobctlDriverPause(void)
{
    threadSleep(jobctlDriverPollMs);
    while (currentThread()->state == threadSleeping)
        halt();
}

static uint32 liveThreadNamed(const char* name)
{
    for (uint32 id = 1; id < threadCount(); id++) {
        const Thread* thread = threadAt(id);
        if ((thread->state != threadUnused && thread->state != threadDead) &&
            strcmp(thread->name, name) == 0)
            return id;
    }
    return threadInvalidId;
}

static uint32 awaitBlockedThreadNamed(const char* name)
{
    uint32 id;
    while ((id = liveThreadNamed(name)) == threadInvalidId || threadAt(id)->state != threadBlocked)
        jobctlDriverPause();
    return id;
}

static void jobctlDriverThread(void)
{
    threadSleep(jobctlDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell commands for the on-demand job-control demo\n");
    jobctlTypeFlushLine();

    jobctlTypeBackgroundCommand();
    uint32 jobctlbgPid = awaitBlockedThreadNamed("/bin/jobctlbg");

    jobctlTypeForegroundCommand();
    uint32 jobctlfgPid = awaitBlockedThreadNamed("/bin/jobctlfg");
    while (keyboardForegroundPgid() != jobctlfgPid)
        jobctlDriverPause();

    kprintf("[kbd]  injecting synthetic ctrl-c for the on-demand job-control demo\n");
    keyboardHandleScancode(scancodeLeftCtrlMake);
    keyboardHandleScancode(scancodeCPressMake);
    keyboardHandleScancode(scancodeCPressBreak);
    keyboardHandleScancode(scancodeLeftCtrlBreak);

    while (keyboardForegroundPgid() == jobctlfgPid)
        jobctlDriverPause();

    kprintf("[user] jobctl demo: jobctlfg pid=%u foreground pgid now=%u -> "
            "shell-foreground-reclaimed-ok\n",
            jobctlfgPid, keyboardForegroundPgid());

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

#define scancodeMakeTwo 0x03

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

#define hereStringPipelineDriverInitialDelayMs 9000
#define hereStringPipelineDriverSettleMs       500

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
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeEnter);
}

static void envTypeEnvCommand(void)
{
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeN);
    injectKeyPress(scancodeMakeV);
    injectKeyPress(scancodeMakeEnter);
}

#define envDriverInitialDelayMs 12500
#define envDriverStepSettleMs   400

static void envDriverThread(void)
{
    threadSleep(envDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell commands for the environment variable demo\n");
    jobctlTypeFlushLine();

    envTypeExportCommand();
    threadSleep(envDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    envTypeEnvprintCommand();
    threadSleep(envDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    envTypeEnvCommand();
    threadSleep(envDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    for (;;)
        halt();
}

#define scancodeMakeEight 0x09

static void globTypeMkdirCommand(void)
{
    injectKeyPress(scancodeMakeM);
    injectKeyPress(scancodeMakeK);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeR);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeM);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeG);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeEnter);
}

static void globTypeEchoACommand(void)
{
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeH);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeA);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeM);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeG);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeA);
    injectKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeX);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeEnter);
}

static void globTypeEchoBCommand(void)
{
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeH);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeB);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeM);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeG);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeB);
    injectKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeX);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeEnter);
}

static void globTypeEchoSkipCommand(void)
{
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeH);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeM);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeG);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeL);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeG);
    injectKeyPress(scancodeMakeEnter);
}

static void globTypeGlobcatCommand(void)
{
    injectKeyPress(scancodeMakeG);
    injectKeyPress(scancodeMakeL);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeB);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeA);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeM);
    injectKeyPress(scancodeMakeP);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeG);
    injectKeyPress(scancodeMakeD);
    injectKeyPress(scancodeMakeSlash);
    injectShiftedKeyPress(scancodeMakeEight);
    injectKeyPress(scancodeMakeDot);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeX);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeEnter);
}

#define globDriverInitialDelayMs 15000
#define globDriverStepSettleMs   300

static void globDriverThread(void)
{
    threadSleep(globDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell commands for the wildcard glob demo\n");
    jobctlTypeFlushLine();

    globTypeMkdirCommand();
    threadSleep(globDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    globTypeEchoACommand();
    threadSleep(globDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    globTypeEchoBCommand();
    threadSleep(globDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    globTypeEchoSkipCommand();
    threadSleep(globDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    globTypeGlobcatCommand();
    threadSleep(globDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    for (;;)
        halt();
}

#define lsDriverInitialDelayMs 19500
#define lsDriverStepSettleMs   300

static void lsTypeLongListCommand(void)
{
    injectKeyPress(scancodeMakeL);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeHyphen);
    injectKeyPress(scancodeMakeL);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeM);
    injectKeyPress(scancodeMakeN);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeSlash);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeI);
    injectKeyPress(scancodeMakeM);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeT);
    injectKeyPress(scancodeMakeEnter);
}

static void lsDriverThread(void)
{
    threadSleep(lsDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell commands for the ls -l demo\n");
    jobctlTypeFlushLine();

    lsTypeLongListCommand();
    threadSleep(lsDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    for (;;)
        halt();
}

#define andOrDriverInitialDelayMs 20500
#define andOrDriverStepSettleMs   300

static void andOrTypeFailChainCommand(void)
{
    injectKeyPress(scancodeMakeF);
    injectKeyPress(scancodeMakeA);
    injectKeyPress(scancodeMakeL);
    injectKeyPress(scancodeMakeS);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeSeven);
    injectShiftedKeyPress(scancodeMakeSeven);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeH);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeX);
    injectKeyPress(scancodeMakeSpace);
    injectShiftedKeyPress(scancodeMakeBackslash);
    injectShiftedKeyPress(scancodeMakeBackslash);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeE);
    injectKeyPress(scancodeCPressMake);
    injectKeyPress(scancodeMakeH);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeSpace);
    injectKeyPress(scancodeMakeA);
    injectKeyPress(scancodeMakeO);
    injectKeyPress(scancodeMakeTwo);
    injectKeyPress(scancodeMakeEnter);
}

static void andOrDriverThread(void)
{
    threadSleep(andOrDriverInitialDelayMs);
    while (currentThread()->state == threadSleeping)
        halt();

    kprintf("[kbd]  typing shell commands for the &&/|| chaining demo\n");
    jobctlTypeFlushLine();

    andOrTypeFailChainCommand();
    threadSleep(andOrDriverStepSettleMs);
    while (currentThread()->state == threadSleeping)
        halt();

    for (;;)
        halt();
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

    sigInstallTrampoline();
    kprintf("[sig]  shared signal trampoline mapped at %p\n", (void*)sigTrampolineVirtualAddr);

    sigInstallInfoPage();
    kprintf("[sig]  shared siginfo page(s) mapped at %p\n", (void*)sigInfoVirtualAddr);

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

    if (vfsMkfifo("/tmp/rendezvous") == 0)
        kprintf("[vfs]  mkfifo /tmp/rendezvous ready\n");
    else
        kprintf("[vfs]  mkfifo /tmp/rendezvous FAILED\n");

    if (ataInit())
        kprintf("[ata]  primary master '%s', %lu sectors (%lu MiB), /dev/disk0 ready\n",
                ataModelString(), ataSectorCount(),
                (ataSectorCount() * ataSectorSize) / (1024 * 1024));
    else
        kprintf("[ata]  no primary master drive detected, /dev/disk0 will read as empty\n");

    if (sfsMount())
        kprintf("[sfs]  mounted persistent filesystem at /mnt (%lu KiB usable)\n",
                sfsCapacityBytes() / 1024);
    else
        kprintf("[sfs]  no usable disk for /mnt (drive missing or too small)\n");

    ktestRunAll();

    threadCreate("shell", shellThread);
    uint32 workerAId = threadCreate("workerA", workerA);
    uint32 workerBId = threadCreate("workerB", workerB);

    threadSetPriority(workerAId, schedDefaultWeight * schedPriorityWeightRatio);
    kprintf("[sched] workerA pid=%u weight=%u, workerB pid=%u weight=%u "
            "(CFS-lite: higher weight runs more often)\n",
            workerAId, schedDefaultWeight * schedPriorityWeightRatio, workerBId, schedDefaultWeight);
    threadCreate("schedVerify", schedPriorityVerifierThread);

    pipeDemoHandle = pipeCreate();
    if (pipeDemoHandle >= 0) {
        threadCreate("pipeProducer", pipeProducerThread);
        threadCreate("pipeConsumer", pipeConsumerThread);
    } else {
        kprintf("[pipe] demo pipe creation failed\n");
    }

    joinDemoWorkerId = threadCreate("joinWorker", shortLivedWorkerThread);
    if (joinDemoWorkerId != threadInvalidId) {
        joinDemoWorkerGeneration = threadAt(joinDemoWorkerId)->generation;
        threadCreate("joiner", joinerThread);
    } else
        kprintf("[sched] join demo worker not started: thread table full\n");

    kprintf("\n[boot] launching userland programs from the ramdisk into ring 3\n");
    spawnUser("/bin/hello", "hello", 0x48000000, 0);
    spawnUser("/bin/counter", "counter", 0x58000000, 0);
    spawnUser("/bin/sbrktest", "sbrktest", 0x68000000, 0x64000000);
    spawnUser("/bin/fileread", "fileread", 0x78000000, 0);
    spawnUser("/bin/pipetest", "pipetest", 0x88000000, 0);
    spawnUser("/bin/disktest", "disktest", 0x98000000, 0);
    spawnUser("/bin/fstest", "fstest", 0xA8000000, 0);
    spawnUser("/bin/linktest", "linktest", 0xB8000000, 0);
    spawnUser("/bin/fifomaster", "fifomaster", 0x118000000, 0);
    spawnUser("/bin/killtest", "killtest", 0x148000000, 0);
    spawnUser("/bin/namedreader", "namedreader", 0x168000000, 0);
    spawnUser("/bin/namedwriter", "namedwriter", 0x178000000, 0);
    spawnUser("/bin/consolereader1", "consolereader1", 0x188000000, 0);
    spawnUser("/bin/consolereader2", "consolereader2", 0x198000000, 0);
    threadCreate("consoleInjector", consoleInjectorThread);
    spawnUser("/bin/sigtest", "sigtest", 0x1A8000000, 0);
    spawnUser("/bin/jobtest", "jobtest", 0x1D8000000, 0);
    spawnUser("/bin/nesttest", "nesttest", 0x1F8000000, 0);
    spawnUser("/bin/eintrtest", "eintrtest", 0x218000000, 0);
    spawnUser("/bin/restarttest", "restarttest", 0x238000000, 0);
    spawnUser("/bin/masktest", "masktest", 0x258000000, 0);
    spawnUser("/bin/sigchldtest", "sigchldtest", 0x278000000, 0);
    spawnUser("/bin/waitanytest", "waitanytest", 0x298000000, 0);
    spawnUser("/bin/stopchldtest", "stopchldtest", 0x2C8000000, 0);
    spawnUser("/bin/nodefertest", "nodefertest", 0x2E8000000, 0);
    spawnUser("/bin/alarmtest", "alarmtest", 0x2F8000000, 0);
    spawnUser("/bin/siginfotest", "siginfotest", 0x318000000, 0);
    spawnUser("/bin/selfstoptest", "selfstoptest", 0x3F8000000, 0);
    spawnUser("/bin/cwdtest", "cwdtest", 0x458000000, 0);
    spawnUser("/bin/nicetest", "nicetest", 0x4A8000000, 0);
    spawnUser("/bin/date", "date", 0x4B8000000, 0);
    spawnUser("/bin/timetest", "timetest", 0x4D8000000, 0);
    spawnUser("/bin/rmtest", "rmtest", 0x508000000, 0);
    spawnUser("/bin/permtest", "permtest", 0x518000000, 0);
    spawnUser("/bin/identtest", "identtest", 0x528000000, 0);
    spawnUser("/bin/dirpermtest", "dirpermtest", 0x538000000, 0);
    spawnUser("/bin/grouptest", "grouptest", 0x548000000, 0);
    spawnUser("/bin/pidfdtest", "pidfdtest", 0x558000000, 0);
    spawnUser("/bin/mmaptest", "mmaptest", 0x568000000, 0);
    spawnUser("/bin/polltest", "polltest", 0x378000000, 0);
    pgroupfgPid = spawnUser("/bin/pgroupfg", "pgroupfg", 0x338000000, 0);
    pgroupbgPid = spawnUser("/bin/pgroupbg", "pgroupbg", 0x348000000, 0);
    if (pgroupfgPid != threadInvalidId) {
        keyboardSetForegroundPgid(pgroupfgPid);
    }
    if (pgroupfgPid != threadInvalidId && pgroupbgPid != threadInvalidId)
        threadCreate("pgroupInjector", pgroupInjectorThread);
    threadCreate("jobctlDriver", jobctlDriverThread);
    threadCreate("pipelineDriver", pipelineDriverThread);
    threadCreate("redirDriver", redirDriverThread);
    threadCreate("stderrDriver", stderrDriverThread);
    threadCreate("hereDriver", hereDriverThread);
    threadCreate("hereStringPipelineDriver", hereStringPipelineDriverThread);
    threadCreate("threeStagePipelineDriver", threeStagePipelineDriverThread);
    threadCreate("envDriver", envDriverThread);
    threadCreate("globDriver", globDriverThread);
    threadCreate("lsDriver", lsDriverThread);
    threadCreate("andOrDriver", andOrDriverThread);
    execDemoPid = spawnUser("/bin/proctest", "proctest", 0xF8000000, 0);
    if (execDemoPid != threadInvalidId)
        threadCreate("execReaper", execReaperThread);

    kprintf("[sched] %u threads total (idle, shell, 2 workers, priority verifier, user programs)\n",
            threadCount());
    kprintf("\n[boot] enabling interrupts, starting preemptive scheduler\n\n");

    schedStart();
}
