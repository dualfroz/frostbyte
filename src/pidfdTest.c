#include "ktest.h"
#include "sched.h"
#include "pidfd.h"
#include "ports.h"

static void noopThreadEntry(void)
{
    for (;;)
        halt();
}

KTEST(pidfdOpenRejectsIdleOutOfRangeAndUnusedTargets)
{
    KT_EQ(t, pidfdOpen(0), -1);
    KT_EQ(t, pidfdOpen(threadCapacity()), -1);
    KT_EQ(t, pidfdOpen(threadCapacity() + 1000), -1);

    uint32 a = threadCreate("pfA", noopThreadEntry);
    uint32 b = threadCreate("pfB", noopThreadEntry);
    uint32 c = threadCreate("pfC", noopThreadEntry);
    KT_TRUE(t, threadDiscard(b));

    KT_EQ(t, pidfdOpen(b), -1);

    KT_TRUE(t, threadDiscard(a));
    KT_TRUE(t, threadDiscard(c));
}

KTEST(pidfdOpenAndTargetRoundtripThenClose)
{
    uint32 x = threadCreate("pfX", noopThreadEntry);

    int handle = pidfdOpen(x);
    KT_GE(t, handle, 0);
    KT_EQU(t, pidfdTarget(handle), x);

    pidfdClose(handle);
    KT_EQU(t, pidfdTarget(handle), threadInvalidId);

    pidfdClose(handle);
    KT_EQU(t, pidfdTarget(handle), threadInvalidId);

    KT_TRUE(t, threadDiscard(x));
}

KTEST(pidfdTargetDetectsDiscardedTarget)
{
    uint32 x = threadCreate("pfDiscarded", noopThreadEntry);
    int handle = pidfdOpen(x);
    KT_GE(t, handle, 0);

    KT_TRUE(t, threadDiscard(x));

    KT_EQU(t, pidfdTarget(handle), threadInvalidId);

    pidfdClose(handle);
}

KTEST(pidfdTargetDetectsSlotRecycledByAnUnrelatedThread)
{
    uint32 capacity = threadCapacity();
    uint32 created[capacity];
    uint32 count = 0;

    while (count < capacity) {
        uint32 id = threadCreate("pfFill", noopThreadEntry);
        if (id == threadInvalidId)
            break;
        created[count++] = id;
    }
    KT_EQU(t, threadCreate("pfShouldFail", noopThreadEntry), threadInvalidId);

    uint32 victim = created[count / 2];
    int handle = pidfdOpen(victim);
    KT_GE(t, handle, 0);
    KT_EQU(t, pidfdTarget(handle), victim);

    KT_TRUE(t, threadDiscard(victim));
    uint32 impostor = threadCreate("pfImpostor", noopThreadEntry);
    KT_EQ(t, impostor, victim);

    KT_EQU(t, pidfdTarget(handle), threadInvalidId);

    int freshHandle = pidfdOpen(impostor);
    KT_GE(t, freshHandle, 0);
    KT_EQU(t, pidfdTarget(freshHandle), impostor);

    pidfdClose(handle);
    pidfdClose(freshHandle);
    for (uint32 i = 0; i < count; i++) {
        if (created[i] != victim)
            KT_TRUE(t, threadDiscard(created[i]));
    }
    KT_TRUE(t, threadDiscard(impostor));
}

KTEST(pidfdOpenExhaustsThenClosesFreeSlotsBackForReuse)
{
    uint32 x = threadCreate("pfTableFill", noopThreadEntry);

    int handles[maxPidfds];
    int count = 0;
    while (count < maxPidfds) {
        int h = pidfdOpen(x);
        if (h < 0)
            break;
        handles[count++] = h;
    }
    KT_EQ(t, count, maxPidfds);
    KT_EQ(t, pidfdOpen(x), -1);

    pidfdClose(handles[0]);
    int reused = pidfdOpen(x);
    KT_GE(t, reused, 0);
    KT_EQU(t, pidfdTarget(reused), x);

    pidfdClose(reused);
    for (int i = 1; i < count; i++)
        pidfdClose(handles[i]);
    KT_TRUE(t, threadDiscard(x));
}
