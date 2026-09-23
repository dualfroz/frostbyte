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
    bool everHadReader;
    bool everHadWriter;
    uint32 incarnation;
} Pipe;

static Pipe pipes[maxPipes];

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

static int createPipe(void)
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

static int createDetachedPipe(void)
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

static void addReader(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return;
    p->readers++;
    p->everHadReader = true;
    schedWakeChannel((const void*)p);
}

bool pipeHasReader(int handle)
{
    Pipe* p = pipeAt(handle);
    return p && p->readers > 0;
}

static void addWriter(int handle)
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

static int64 readPipe(int handle, void* buffer, uint64 length)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return -1;
    if (length == 0)
        return 0;

    if (!ringIsEmpty(&p->ring)) {
        usize got = ringRead(&p->ring, (uint8*)buffer, (usize)length);
        schedWakeChannel((const void*)p);
        schedWakeChannel(schedPollChannel());
        return (int64)got;
    }

    if (p->writers == 0 && p->everHadWriter)
        return 0;

    return -1;
}

static int64 writePipe(int handle, const void* buffer, uint64 length)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return -1;
    if (p->readers == 0 && p->everHadReader)
        return -1;
    if (length == 0)
        return 0;

    if (ringIsFull(&p->ring))
        return -1;

    usize put = ringWrite(&p->ring, (const uint8*)buffer, (usize)length);
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
        schedBlockCurrent((const void*)p);
        int64 result = pipeRead(handle, buffer, length);
        if (result != -1) {
            schedCancelBlockCurrent();
            return result;
        }
        halt();
    }
}

int64 pipeWriteBlocking(int handle, const void* buffer, uint64 length)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return -1;

    for (;;) {
        schedBlockCurrent((const void*)p);
        int64 result = pipeWrite(handle, buffer, length);
        if (result != -1 || pipeWriteBroken(handle)) {
            schedCancelBlockCurrent();
            return result;
        }
        halt();
    }
}

static void pipeReleaseIfUnused(int handle)
{
    Pipe* p = &pipes[handle];
    if (p->readers == 0 && p->writers == 0)
        p->used = false;
}

static void closeRead(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return;
    if (p->readers > 0)
        p->readers--;
    if (p->readers == 0) {
        schedWakeChannel((const void*)p);
        schedWakeChannel(schedPollChannel());
    }
    pipeReleaseIfUnused(handle);
}

static void closeWrite(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p)
        return;
    if (p->writers > 0)
        p->writers--;
    if (p->writers == 0) {
        schedWakeChannel((const void*)p);
        schedWakeChannel(schedPollChannel());
    }
    pipeReleaseIfUnused(handle);
}

static bool destroyIfUnattached(int handle)
{
    Pipe* p = pipeAt(handle);
    if (!p || p->readers != 0 || p->writers != 0)
        return false;
    p->used = false;
    return true;
}

int pipeCreate(void)
{
    uint64 flags = saveAndDisableInterrupts();
    int handle = createPipe();
    restoreInterrupts(flags);
    return handle;
}

int pipeCreateDetached(void)
{
    uint64 flags = saveAndDisableInterrupts();
    int handle = createDetachedPipe();
    restoreInterrupts(flags);
    return handle;
}

void pipeAddReader(int handle)
{
    uint64 flags = saveAndDisableInterrupts();
    addReader(handle);
    restoreInterrupts(flags);
}

void pipeAddWriter(int handle)
{
    uint64 flags = saveAndDisableInterrupts();
    addWriter(handle);
    restoreInterrupts(flags);
}

int64 pipeRead(int handle, void* buffer, uint64 length)
{
    uint64 flags = saveAndDisableInterrupts();
    int64 got = readPipe(handle, buffer, length);
    restoreInterrupts(flags);
    return got;
}

int64 pipeWrite(int handle, const void* buffer, uint64 length)
{
    uint64 flags = saveAndDisableInterrupts();
    int64 put = writePipe(handle, buffer, length);
    restoreInterrupts(flags);
    return put;
}

void pipeCloseRead(int handle)
{
    uint64 flags = saveAndDisableInterrupts();
    closeRead(handle);
    restoreInterrupts(flags);
}

void pipeCloseWrite(int handle)
{
    uint64 flags = saveAndDisableInterrupts();
    closeWrite(handle);
    restoreInterrupts(flags);
}

bool pipeDestroyIfUnattached(int handle)
{
    uint64 flags = saveAndDisableInterrupts();
    bool destroyed = destroyIfUnattached(handle);
    restoreInterrupts(flags);
    return destroyed;
}
