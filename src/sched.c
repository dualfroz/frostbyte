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

#define maxThreads 16
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
