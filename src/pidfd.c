#include "pidfd.h"
#include "sched.h"

typedef struct PidfdEntry {
    bool used;
    uint32 targetId;
    uint32 targetGeneration;
} PidfdEntry;

static PidfdEntry pidfds[maxPidfds];

static PidfdEntry* pidfdAt(int handle)
{
    if (handle < 0 || handle >= (int)maxPidfds)
        return nullptr;
    if (!pidfds[handle].used)
        return nullptr;
    return &pidfds[handle];
}

static int allocPidfdSlot(void)
{
    for (int i = 0; i < (int)maxPidfds; i++)
        if (!pidfds[i].used)
            return i;
    return -1;
}

int pidfdOpen(uint32 targetId)
{
    if (targetId == 0 || targetId >= threadCapacity())
        return -1;
    if (threadAt(targetId)->state == threadUnused)
        return -1;

    int slot = allocPidfdSlot();
    if (slot < 0)
        return -1;

    pidfds[slot].used = true;
    pidfds[slot].targetId = targetId;
    pidfds[slot].targetGeneration = threadAt(targetId)->generation;
    return slot;
}

uint32 pidfdTarget(int handle)
{
    PidfdEntry* p = pidfdAt(handle);
    if (!p)
        return threadInvalidId;
    /* targetId was range/liveness-checked once, at pidfdOpen time, and
       never changes afterward -- threadAt is safe here without repeating
       that check. A mismatch means the slot has since been freed
       (threadReap/threadDiscard's memset zeroes Thread.generation back to
       0) or handed to an entirely different, unrelated thread. */
    if (threadAt(p->targetId)->generation != p->targetGeneration)
        return threadInvalidId;
    return p->targetId;
}

void pidfdClose(int handle)
{
    PidfdEntry* p = pidfdAt(handle);
    if (!p)
        return;
    p->used = false;
}
