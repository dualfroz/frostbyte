#include "pipe.h"
#include "ring.h"
#include "sched.h"
#include "ports.h"

typedef struct Pipe {
    RingBuffer ring;
    uint8 backing[pipeBufferSize];
    uint32 readers;
    uint32 writers;
    bool used;
    /* Set once, permanently (mirrors Thread.everRan, sched.h), the first
       time a reader/writer is ever attached. A pipeCreateDetached handle
       starts with both counts AND both of these false, so pipeRead/
       pipeWrite can tell "the other end just has not attached yet" (keep
       retrying) apart from "it attached and then went away" (true EOF /
       broken pipe) -- pipeCreate sets both true immediately, since its
       caller always installs both ends atomically, so its behavior is
       completely unchanged. */
    bool everHadReader;
    bool everHadWriter;
    /* Distinguishes THIS specific pipe living at a given slot from any other
       pipe that has occupied (or will ever occupy) the same numeric handle
       before or after -- pipeReleaseIfUnused frees a slot's `used` flag the
       moment both ends are gone, so allocPipeSlot happily reuses that same
       number for a completely unrelated pipeCreate/pipeCreateDetached right
       after. A caller that caches a handle across time spanning such a
       release (vfs.c's SFS-backed named-fifo bindings, see sfsFifoHandleFor
       in vfs.c) needs more than pipeValid(handle) to tell "the same pipe I
       bound to earlier" apart from "a different pipe that happens to have
       been assigned the same number since" -- see pipeIncarnation. */
    uint32 incarnation;
} Pipe;

static Pipe pipes[maxPipes];

/* Never 0 (see pipeIncarnation's "0 always means invalid" contract) -- next
   value handed to whichever pipe allocPipeSlot's caller creates next,
   regardless of which slot index it lands in. */
static uint32 nextPipeIncarnation = 1;

static Pipe* pipeAt(int handle)
{
    if (handle < 0 || handle >= (int)maxPipes)
        return nullptr;
    if (!pipes[handle].used)
        return nullptr;
    return &pipes[handle];
}

static int allocPipeSlot(void)
{
    for (int i = 0; i < (int)maxPipes; i++)
        if (!pipes[i].used)
            return i;
    return -1;
}

int pipeCreate(void)
{
    int i = allocPipeSlot();
    if (i < 0)
        return -1;

    Pipe* p = &pipes[i];
    ringInit(&p->ring, p->backing, pipeBufferSize);
    p->readers = 1;
    p->writers = 1;
    p->everHadReader = true;
    p->everHadWriter = true;
    p->used = true;
    p->incarnation = nextPipeIncarnation++;
    return i;
}

int pipeCreateDetached(void)
{
    int i = allocPipeSlot();
    if (i < 0)
        return -1;

    Pipe* p = &pipes[i];
    ringInit(&p->ring, p->backing, pipeBufferSize);
    p->readers = 0;
    p->writers = 0;
    p->everHadReader = false;
    p->everHadWriter = false;
    p->used = true;
    p->incarnation = nextPipeIncarnation++;
    return i;
}

void pipeAddReader(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return;
    p->readers++;
    p->everHadReader = true;
}

void pipeAddWriter(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return;
    p->writers++;
    p->everHadWriter = true;
}

bool pipeWriteBroken(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return true;
    return p->readers == 0 && p->everHadReader;
}

const void* pipeChannel(int handle)
{
    return (const void*)pipeAt(handle);
}

bool pipeValid(int handle)
{
    return pipeAt(handle) != nullptr;
}

uint32 pipeIncarnation(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return 0;
    return p->incarnation;
}

int64 pipeRead(int handle, void* buffer, uint64 length)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return -1;
    if (length == 0)
        return 0;

    if (!ringIsEmpty(&p->ring)) {
        usize got = ringRead(&p->ring, (uint8*)buffer, (usize)length);
        /* Reading frees ring space: any writer blocked on "buffer full" may
           now be able to proceed, so give it a chance to recheck. */
        schedWakeChannel((const void*)p);
        schedWakeChannel(schedPollChannel());
        return (int64)got;
    }

    /* Only a writer that WAS attached and has since gone away means true
       EOF -- a pipeCreateDetached handle nobody has written to yet (readers
       0, everHadWriter false) has to keep reporting "try again" instead,
       or a reader that races ahead of the writer's own vfsPipeAttach would
       see spurious end-of-file on an otherwise perfectly live pipe. */
    if (p->writers == 0 && p->everHadWriter)
        return 0;

    return -1;
}

int64 pipeWrite(int handle, const void* buffer, uint64 length)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return -1;
    /* Symmetric with pipeRead's EOF check above: only a reader that WAS
       attached and left counts as a broken pipe. */
    if (p->readers == 0 && p->everHadReader)
        return -1;
    if (length == 0)
        return 0;

    if (ringIsFull(&p->ring))
        return -1;

    usize put = ringWrite(&p->ring, (const uint8*)buffer, (usize)length);
    /* Writing adds ring data: any reader blocked on "buffer empty" may now
       be able to proceed, so give it a chance to recheck. */
    schedWakeChannel((const void*)p);
    schedWakeChannel(schedPollChannel());
    return (int64)put;
}

bool pipeHasData(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return false;
    return !ringIsEmpty(&p->ring) || (p->writers == 0 && p->everHadWriter);
}

bool pipeHasSpace(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return false;
    return !ringIsFull(&p->ring);
}

int64 pipeReadBlocking(int handle, void* buffer, uint64 length)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return -1;

    for (;;) {
        int64 result = pipeRead(handle, buffer, length);
        if (result != -1)
            return result;

        schedBlockCurrent((const void*)p);
        halt();
    }
}

int64 pipeWriteBlocking(int handle, const void* buffer, uint64 length)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return -1;

    for (;;) {
        int64 result = pipeWrite(handle, buffer, length);
        if (result != -1)
            return result;

        /* A reader that attached and then left is a broken pipe: a terminal
           condition no amount of waiting will ever resolve, unlike a merely
           full buffer (or a pipeCreateDetached handle whose reader simply
           has not attached yet). */
        if (pipeWriteBroken(handle))
            return -1;

        schedBlockCurrent((const void*)p);
        halt();
    }
}

static void pipeReleaseIfUnused(int handle)
{
    Pipe* p = &pipes[handle];
    if (p->readers == 0 && p->writers == 0)
        p->used = false;
}

void pipeCloseRead(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return;
    if (p->readers > 0)
        p->readers--;
    /* A writer parked on "buffer full" must be woken to discover the pipe is
       now broken instead of waiting forever for space that will never free
       up on its own. */
    if (p->readers == 0) {
        schedWakeChannel((const void*)p);
        schedWakeChannel(schedPollChannel());
    }
    pipeReleaseIfUnused(handle);
}

void pipeCloseWrite(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return;
    if (p->writers > 0)
        p->writers--;
    /* A reader parked on "buffer empty" must be woken to discover true
       end-of-file instead of waiting forever for data that will never
       arrive. */
    if (p->writers == 0) {
        schedWakeChannel((const void*)p);
        schedWakeChannel(schedPollChannel());
    }
    pipeReleaseIfUnused(handle);
}

bool pipeDestroyIfUnattached(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p || p->readers != 0 || p->writers != 0)
        return false;
    p->used = false;
    return true;
}
