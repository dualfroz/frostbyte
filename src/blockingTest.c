#include "ktest.h"
#include "pipe.h"
#include "sched.h"
#include "heap.h"
#include "pmm.h"
#include "ports.h"
#include "signal.h"

#define interruptFlag 0x200ull

static int attachedPipe(void)
{
    int handle = pipeCreateDetached();
    if (handle >= 0) {
        pipeAddReader(handle);
        pipeAddWriter(handle);
    }
    return handle;
}

static uint64 currentInterruptFlag(void)
{
    uint64 flags = saveAndDisableInterrupts();
    restoreInterrupts(flags);
    return flags & interruptFlag;
}

KTEST(schedCancelBlockCurrentReturnsTheCallerToRunning)
{
    int channel = 0;
    schedBlockCurrent(&channel);
    KT_EQ(t, currentThread()->state, threadBlocked);

    schedCancelBlockCurrent();
    KT_EQ(t, currentThread()->state, threadRunning);
    KT_NULL(t, currentThread()->waitChannel);
}

KTEST(pipeReadBlockingWithDataLeavesTheCallerRunning)
{
    int handle = attachedPipe();
    KT_GE(t, handle, 0);
    KT_EQ(t, pipeWrite(handle, "ab", 2), 2);

    char got[4] = { 0 };
    KT_EQ(t, pipeReadBlocking(handle, got, sizeof(got)), 2);
    KT_EQ(t, got[0], 'a');
    KT_EQ(t, got[1], 'b');
    KT_EQ(t, currentThread()->state, threadRunning);
    KT_NULL(t, currentThread()->waitChannel);

    pipeCloseRead(handle);
    pipeCloseWrite(handle);
    KT_FALSE(t, pipeValid(handle));
}

KTEST(pipeReadBlockingAtEndOfFileReturnsZeroAndLeavesTheCallerRunning)
{
    int handle = attachedPipe();
    KT_GE(t, handle, 0);
    pipeCloseWrite(handle);

    char got[4];
    KT_EQ(t, pipeReadBlocking(handle, got, sizeof(got)), 0);
    KT_EQ(t, currentThread()->state, threadRunning);
    KT_NULL(t, currentThread()->waitChannel);

    pipeCloseRead(handle);
    KT_FALSE(t, pipeValid(handle));
}

KTEST(pipeWriteBlockingOnABrokenPipeFailsWithoutLeavingTheCallerBlocked)
{
    int handle = attachedPipe();
    KT_GE(t, handle, 0);
    pipeCloseRead(handle);
    KT_TRUE(t, pipeWriteBroken(handle));

    KT_EQ(t, pipeWriteBlocking(handle, "x", 1), -1);
    KT_EQ(t, currentThread()->state, threadRunning);
    KT_NULL(t, currentThread()->waitChannel);

    pipeCloseWrite(handle);
    KT_FALSE(t, pipeValid(handle));
}

KTEST(criticalSectionsPreserveTheCallersInterruptFlag)
{
    uint64 before = currentInterruptFlag();

    void* memory = kmalloc(32);
    KT_NOTNULL(t, memory);
    kfree(memory);
    KT_EQU(t, currentInterruptFlag(), before);

    uint64 frame = pmmAlloc();
    KT_NE(t, frame, 0);
    pmmFree(frame);
    KT_EQU(t, currentInterruptFlag(), before);

    int handle = pipeCreateDetached();
    KT_GE(t, handle, 0);
    KT_TRUE(t, pipeDestroyIfUnattached(handle));
    KT_EQU(t, currentInterruptFlag(), before);

    KT_FALSE(t, sigRaise(0, sigUsr1));
    KT_FALSE(t, threadWakeForSignal(0));
    KT_EQU(t, currentInterruptFlag(), before);
}
