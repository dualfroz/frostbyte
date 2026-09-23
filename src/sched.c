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

#define maxThreads 288
#define threadStackSize 16384

#define schedTickCharge 1000000ull

static Thread threads[maxThreads];
static uint32 threadTotal;
static uint32 currentIndex;
static bool schedulerActive;
static uint64 minVruntime;

static uint32 nextSlotHint = 1;

static uint32 nextThreadGeneration = 1;

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

static uint32 initialPgid(uint32 newId)
{
    if (currentIndex == 0)
        return newId;
    return threads[currentIndex].pgid;
}

static uint32 initialUid(uint32 newId)
{
    (void)newId;
    if (currentIndex == 0)
        return 0;
    return threads[currentIndex].uid;
}

static uint32 initialGid(uint32 newId)
{
    (void)newId;
    if (currentIndex == 0)
        return 0;
    return threads[currentIndex].gid;
}

static void initialCwd(uint32 newId)
{
    if (currentIndex == 0) {
        threads[newId].cwd[0] = '/';
        threads[newId].cwd[1] = 0;
        return;
    }
    memcpy(threads[newId].cwd, threads[currentIndex].cwd, fsPathMax);
}

static void initialEnv(uint32 newId)
{
    if (currentIndex == 0) {
        threads[newId].envCount = 0;
        return;
    }
    memcpy(threads[newId].env, threads[currentIndex].env, sizeof(threads[newId].env));
    threads[newId].envCount = threads[currentIndex].envCount;
}

static uint32 createKernelThread(const char* name, ThreadEntry entry)
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

uint32 threadCreate(const char* name, ThreadEntry entry)
{
    uint64 flags = saveAndDisableInterrupts();
    uint32 slot = createKernelThread(name, entry);
    restoreInterrupts(flags);
    return slot;
}

static uint32 createUserThread(const char* name, uint64 entry, uint64 userStackTop)
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

uint32 threadCreateUser(const char* name, uint64 entry, uint64 userStackTop)
{
    uint64 flags = saveAndDisableInterrupts();
    uint32 slot = createUserThread(name, entry, userStackTop);
    restoreInterrupts(flags);
    return slot;
}

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

    for (uint32 i = 0; i < threadTotal; i++)
        if (threads[i].state == threadBlocked && threads[i].pollActive &&
            threads[i].pollDeadline != 0 && now >= threads[i].pollDeadline)
            threads[i].state = threadRunnable;

    for (uint32 i = 0; i < threadTotal; i++)
        if (threads[i].alarmAt != 0 && now >= threads[i].alarmAt) {
            threads[i].alarmAt = 0;
            sigRaiseInfo(threads[i].id, sigAlrm, siCodeKernel, 0, 0);
        }

    PrioQueue ready;
    prioqInit(&ready);
    for (uint32 i = 0; i < threadTotal; i++)
        if (threads[i].state == threadRunnable) {
            if (threads[i].vruntime < minVruntime)
                threads[i].vruntime = minVruntime;
            prioqInsert(&ready, &threads[i].schedNode, threads[i].vruntime, threads[i].id);
        }

    PrioQNode* winner = prioqPopMin(&ready);
    if (winner)
        minVruntime = winner->vruntime;
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
    uint64 flags = saveAndDisableInterrupts();
    Thread* thread = &threads[currentIndex];
    thread->exitCode = code;
    thread->state = threadDead;
    kprintf("[user] pid=%u '%s' exited with code %lu\n", thread->id, thread->name, code);

    sigRaiseInfo(thread->parentId, sigChld, siCodeChldExited, (int)thread->id, (int)code);

    schedWakeChannel(thread);

    schedWakeChannel(&threads[thread->parentId]);
    restoreInterrupts(flags);
}

bool threadKill(uint32 id)
{
    if (id == 0 || id >= maxThreads)
        return false;

    uint64 flags = saveAndDisableInterrupts();
    ThreadState state = threads[id].state;
    if (state != threadRunnable && state != threadSleeping &&
        state != threadBlocked && state != threadStopped) {
        restoreInterrupts(flags);
        return false;
    }

    threads[id].exitCode = threadKilledExitCode;
    threads[id].state = threadDead;
    kprintf("[sched] pid=%u '%s' killed\n", id, threads[id].name);

    sigRaiseInfo(threads[id].parentId, sigChld, siCodeChldKilled, (int)id, (int)threadKilledExitCode);

    schedWakeChannel(&threads[id]);

    schedWakeChannel(&threads[threads[id].parentId]);
    restoreInterrupts(flags);
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
    if (threads[currentIndex].uid != 0) {
        uint32 gid = threads[currentIndex].gid;
        if (newGid != gid)
            return false;
    }
    threads[currentIndex].gid = newGid;
    return true;
}

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
    return nullptr;
}

bool currentThreadSetEnv(const char* name, const char* value)
{
    Thread* self = &threads[currentIndex];
    uint64 nameLen = strlen(name);
    uint64 valueLen = strlen(value);
    if (nameLen + 1 + valueLen + 1 > envEntryMax)
        return false;

    int slot = -1;
    for (int i = 0; i < self->envCount; i++) {
        if (envNameMatches(self->env[i], name)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (self->envCount >= envMaxVars)
            return false;
        slot = self->envCount++;
    }

    memcpy(self->env[slot], name, nameLen);
    self->env[slot][nameLen] = '=';
    memcpy(self->env[slot] + nameLen + 1, value, valueLen);
    self->env[slot][nameLen + 1 + valueLen] = 0;
    return true;
}

bool currentThreadUnsetEnv(const char* name)
{
    Thread* self = &threads[currentIndex];
    int slot = -1;
    for (int i = 0; i < self->envCount; i++) {
        if (envNameMatches(self->env[i], name)) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return false;

    int last = self->envCount - 1;
    if (slot != last)
        memmove(self->env[slot], self->env[slot + 1], (uint64)(last - slot) * envEntryMax);
    self->envCount--;
    return true;
}

int currentThreadEnvCount(void)
{
    return threads[currentIndex].envCount;
}

const char* currentThreadEnvEntry(int index)
{
    if (index < 0 || index >= threads[currentIndex].envCount)
        return nullptr;
    return threads[currentIndex].env[index];
}

static bool stopThread(uint32 id)
{
    ThreadState state = threads[id].state;
    if (state == threadDead || state == threadUnused)
        return false;
    if (state == threadStopped) {
        return true;
    }

    threads[id].state = threadStopped;
    kprintf("[sched] pid=%u '%s' stopped\n", id, threads[id].name);

    sigRaiseInfo(threads[id].parentId, sigChld, siCodeChldStopped, (int)id, sigStop);
    return true;
}

bool threadStop(uint32 id)
{
    if (id == 0 || id >= maxThreads)
        return false;

    uint64 flags = saveAndDisableInterrupts();
    bool stopped = stopThread(id);
    restoreInterrupts(flags);
    return stopped;
}

bool threadContinue(uint32 id)
{
    if (id == 0 || id >= maxThreads)
        return false;

    uint64 flags = saveAndDisableInterrupts();
    if (threads[id].state != threadStopped) {
        restoreInterrupts(flags);
        return false;
    }

    threads[id].state = threadRunnable;
    kprintf("[sched] pid=%u '%s' continued\n", id, threads[id].name);

    sigRaiseInfo(threads[id].parentId, sigChld, siCodeChldContinued, (int)id, sigCont);
    restoreInterrupts(flags);
    return true;
}

bool threadWakeForSignal(uint32 id)
{
    if (id == 0 || id >= maxThreads)
        return false;

    uint64 flags = saveAndDisableInterrupts();
    ThreadState state = threads[id].state;
    if (state != threadSleeping && state != threadBlocked) {
        restoreInterrupts(flags);
        return false;
    }

    threads[id].state = threadRunnable;
    kprintf("[sig]  pid=%u '%s' woken early for a signal\n", id, threads[id].name);
    restoreInterrupts(flags);
    return true;
}

uint32 threadCapacity(void)
{
    return maxThreads;
}

static void reclaimUserSlot(uint32 id, const Thread* thread)
{
    if (thread->userSlotBase == 0)
        return;

    uint64 slotBase = thread->userSlotBase;
    uint64 stackProbe = slotBase + elfSlotStackOffset - 1;
    bool wasMapped = pagingResolve(stackProbe) != 0;

    pagingUnmapUserRange(slotBase, elfSlotSize);

    bool nowFreed = pagingResolve(stackProbe) == 0;
    kprintf("[sched] reap pid=%u freed user slot %p (was-mapped=%s now-freed=%s) -> %s\n",
            id, (void*)slotBase, wasMapped ? "yes" : "no", nowFreed ? "yes" : "no",
            (wasMapped && nowFreed) ? "user-mem-reclaimed-ok" : "user-mem-reclaimed-FAIL");
}

static void reclaimOpenFds(const Thread* thread)
{
    for (uint32 i = 0; i < maxOpenFiles; i++) {
        const FileDescriptor* fd = &thread->fds[i];
        if (fd->kind == fdConsole && fd->pipeHandle >= 0)
            keyboardCloseReader(fd->pipeHandle);
        else if (fd->kind == fdPipeRead)
            pipeCloseRead(fd->pipeHandle);
        else if (fd->kind == fdPipeWrite)
            pipeCloseWrite(fd->pipeHandle);
        else if (fd->kind == fdPidfd)
            pidfdClose(fd->pipeHandle);
    }
}

static void releaseThread(uint32 id)
{
    reclaimOpenFds(&threads[id]);
    if (threads[id].stackBase)
        kfree((void*)threads[id].stackBase);
    memset(&threads[id], 0, sizeof(Thread));
}

bool threadReap(uint32 id)
{
    if (id == 0 || id >= maxThreads)
        return false;

    uint64 flags = saveAndDisableInterrupts();
    bool reaped = threads[id].state == threadDead;
    if (reaped) {
        reclaimUserSlot(id, &threads[id]);
        releaseThread(id);
    }
    restoreInterrupts(flags);
    return reaped;
}

bool threadDiscard(uint32 id)
{
    if (id == 0 || id >= maxThreads)
        return false;

    uint64 flags = saveAndDisableInterrupts();
    bool discarded = threads[id].state == threadRunnable && !threads[id].everRan;
    if (discarded) {
        if (threads[id].userSlotBase != 0)
            pagingUnmapUserRange(threads[id].userSlotBase, elfSlotSize);
        releaseThread(id);
    }
    restoreInterrupts(flags);
    return discarded;
}

int64 threadJoin(uint32 id)
{
    if (id == 0 || id >= maxThreads)
        return -1;

    for (;;) {
        schedBlockCurrent(&threads[id]);
        ThreadState state = threads[id].state;
        if (state == threadUnused || state == threadDead) {
            schedCancelBlockCurrent();
            if (state == threadUnused)
                return -1;
            break;
        }
        halt();
    }

    uint64 code = threads[id].exitCode;
    threadReap(id);
    return (int64)code;
}

WaitScanResult threadJoinAnyChild(uint32 parentId, uint32* outId, int64* outCode)
{
    bool hasChild = false;
    for (uint32 i = 1; i < threadTotal; i++) {
        if (threads[i].state == threadUnused || threads[i].parentId != parentId)
            continue;
        hasChild = true;
        if (threads[i].state == threadDead) {
            *outId = i;
            *outCode = (int64)threads[i].exitCode;
            threadReap(i);
            return waitScanFoundDead;
        }
    }
    return hasChild ? waitScanNoneDead : waitScanNoChildren;
}

void threadSleep(uint64 milliseconds)
{
    uint64 ticks = milliseconds * pitFrequency() / 1000;
    if (ticks == 0)
        ticks = 1;
    threads[currentIndex].sleepUntil = pitTicks() + ticks;
    threads[currentIndex].state = threadSleeping;
}

uint64 threadSetAlarm(uint64 seconds)
{
    Thread* self = &threads[currentIndex];
    uint64 now = pitTicks();

    uint64 previousRemaining = 0;
    if (self->alarmAt != 0 && self->alarmAt > now) {
        uint64 remainingTicks = self->alarmAt - now;
        uint32 freq = pitFrequency();
        previousRemaining = (remainingTicks + freq - 1) / freq;
    }

    if (seconds == 0)
        self->alarmAt = 0;
    else
        self->alarmAt = now + seconds * (uint64)pitFrequency();

    return previousRemaining;
}

void schedBlockCurrent(const void* channel)
{
    threads[currentIndex].waitChannel = channel;
    threads[currentIndex].state = threadBlocked;
}

const void* schedPollChannel(void)
{
    static const int pollChannelToken;
    return (const void*)&pollChannelToken;
}

void schedCancelBlockCurrent(void)
{
    threads[currentIndex].waitChannel = nullptr;
    threads[currentIndex].state = threadRunning;
}

void schedWakeChannel(const void* channel)
{
    uint64 flags = saveAndDisableInterrupts();
    for (uint32 i = 0; i < threadTotal; i++) {
        if (threads[i].state == threadBlocked && threads[i].waitChannel == channel) {
            threads[i].waitChannel = nullptr;
            threads[i].state = threadRunnable;
        }
    }
    restoreInterrupts(flags);
}

void threadSetBrk(uint32 id, uint64 brk)
{
    if (id >= maxThreads)
        return;
    threads[id].brk = brk;
}

uint64 threadUserSlotBase(uint32 id)
{
    if (id >= maxThreads)
        return 0;
    return threads[id].userSlotBase;
}

void threadSetUserSlotBase(uint32 id, uint64 base)
{
    if (id >= maxThreads)
        return;
    threads[id].userSlotBase = base;
}

bool threadUserSlotInUse(uint64 slotBase)
{
    return threadUserSlotHeldByOther(slotBase, threadInvalidId);
}

bool threadUserSlotHeldByOther(uint64 slotBase, uint32 exceptId)
{
    for (uint32 i = 0; i < threadTotal; i++) {
        if (i != exceptId && threads[i].state != threadUnused && threads[i].userSlotBase == slotBase)
            return true;
    }
    return false;
}

bool threadImageSlotBusy(const void* image, uint64 imageSize, uint32 exceptId)
{
    uint64 entry = elfImageEntry(image, imageSize);
    return entry != 0 && threadUserSlotHeldByOther(elfSlotBase(entry), exceptId);
}

bool threadSetPriority(uint32 id, uint32 weight)
{
    if (id == 0 || id >= maxThreads || weight == 0)
        return false;
    ThreadState state = threads[id].state;
    if (state == threadDead || state == threadUnused)
        return false;

    threads[id].weight = weight;
    return true;
}

uint32 threadPriority(uint32 id)
{
    if (id >= maxThreads || threads[id].state == threadUnused)
        return 0;
    return threads[id].weight;
}

uint64 threadVruntime(uint32 id)
{
    if (id >= threadTotal)
        return 0;
    return threads[id].vruntime;
}

uint64 currentThreadSbrk(int64 increment)
{
    Thread* thread = &threads[currentIndex];
    uint64 previous = thread->brk;

    if (increment > 0)
        pagingMapUserRangeFlags(previous, (uint64)increment, pageNoExecute);

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
