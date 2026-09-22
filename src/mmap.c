#include "mmap.h"
#include "sched.h"
#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "vfs.h"
#include "string.h"

static uint64 pageAlignDown(uint64 x)
{
    return x & ~(uint64)(frameSize - 1);
}

static uint64 pageAlignUp(uint64 x)
{
    return (x + frameSize - 1) & ~(uint64)(frameSize - 1);
}

static uint64 mmapRegionStart(struct Thread* thread)
{
    return thread->userSlotBase + elfSlotStackOffset;
}

static uint64 mmapRegionEnd(struct Thread* thread)
{
    return thread->userSlotBase + elfSlotSize;
}

static bool rangesOverlap(uint64 aStart, uint64 aEnd, uint64 bStart, uint64 bEnd)
{
    return aStart < bEnd && bStart < aEnd;
}

Vma* mmapFind(struct Thread* thread, uint64 addr)
{
    for (int i = 0; i < maxVmasPerThread; i++) {
        Vma* v = &thread->vmas[i];
        if (v->used && addr >= v->start && addr < v->end)
            return v;
    }
    return nullptr;
}

static Vma* findFreeVmaSlot(struct Thread* thread)
{
    for (int i = 0; i < maxVmasPerThread; i++)
        if (!thread->vmas[i].used)
            return &thread->vmas[i];
    return nullptr;
}

static bool regionFree(struct Thread* thread, uint64 start, uint64 end)
{
    for (int i = 0; i < maxVmasPerThread; i++) {
        Vma* v = &thread->vmas[i];
        if (v->used && rangesOverlap(start, end, v->start, v->end))
            return false;
    }
    return true;
}

static uint64 findFreeRegion(struct Thread* thread, uint64 length)
{
    uint64 regionStart = mmapRegionStart(thread);
    uint64 regionEnd = mmapRegionEnd(thread);

    uint64 candidates[maxVmasPerThread + 1];
    int candidateCount = 0;
    candidates[candidateCount++] = regionStart;
    for (int i = 0; i < maxVmasPerThread; i++)
        if (thread->vmas[i].used)
            candidates[candidateCount++] = thread->vmas[i].end;

    for (int i = 1; i < candidateCount; i++) {
        uint64 key = candidates[i];
        int j = i - 1;
        while (j >= 0 && candidates[j] > key) {
            candidates[j + 1] = candidates[j];
            j--;
        }
        candidates[j + 1] = key;
    }

    for (int i = 0; i < candidateCount; i++) {
        uint64 candidate = candidates[i];
        if (candidate < regionStart)
            continue;
        if (candidate + length < candidate || candidate + length > regionEnd)
            continue;
        if (regionFree(thread, candidate, candidate + length))
            return candidate;
    }
    return 0;
}

bool mmapReserve(struct Thread* thread, uint64 addr, uint64 length, uint32 prot,
                  bool anonymous, int fd, uint64 fileOffset, uint64* outAddr)
{
    if (length == 0)
        return false;
    if (addr % frameSize != 0 || length % frameSize != 0)
        return false;
    if (!anonymous && fileOffset % frameSize != 0)
        return false;

    uint64 start = addr;
    if (start == 0) {
        start = findFreeRegion(thread, length);
        if (start == 0)
            return false;
    }

    uint64 end = start + length;
    if (end < start)
        return false;
    if (start < mmapRegionStart(thread) || end > mmapRegionEnd(thread))
        return false;
    if (!regionFree(thread, start, end))
        return false;

    Vma* slot = findFreeVmaSlot(thread);
    if (!slot)
        return false;

    slot->used = true;
    slot->start = start;
    slot->end = end;
    slot->prot = prot;
    slot->anonymous = anonymous;
    slot->fd = anonymous ? -1 : fd;
    slot->fileOffset = anonymous ? 0 : fileOffset;

    *outAddr = start;
    return true;
}

bool mmapRelease(struct Thread* thread, uint64 addr, uint64 length)
{
    if (length == 0 || addr % frameSize != 0)
        return false;

    uint64 unmapStart = addr;
    uint64 unmapEnd = addr + pageAlignUp(length);
    if (unmapEnd < unmapStart)
        return false;

    for (int i = 0; i < maxVmasPerThread; i++) {
        Vma* v = &thread->vmas[i];
        if (v->used && v->start < unmapStart && v->end > unmapEnd && !findFreeVmaSlot(thread))
            return false;
    }

    for (int i = 0; i < maxVmasPerThread; i++) {
        Vma* v = &thread->vmas[i];
        if (!v->used || !rangesOverlap(v->start, v->end, unmapStart, unmapEnd))
            continue;

        uint64 freeFrom = v->start > unmapStart ? v->start : unmapStart;
        uint64 freeTo = v->end < unmapEnd ? v->end : unmapEnd;
        pagingUnmapUserRange(freeFrom, freeTo - freeFrom);

        if (v->start >= unmapStart && v->end <= unmapEnd) {
            v->used = false;
        } else if (v->start < unmapStart && v->end > unmapEnd) {
            Vma* upper = findFreeVmaSlot(thread);
            *upper = *v;
            upper->start = unmapEnd;
            v->end = unmapStart;
        } else if (v->start < unmapStart) {
            v->end = unmapStart;
        } else {
            uint64 trimmed = unmapEnd - v->start;
            if (!v->anonymous)
                v->fileOffset += trimmed;
            v->start = unmapEnd;
        }
    }
    return true;
}

bool mmapFaultHandler(struct Thread* thread, uint64 faultAddr, bool forWrite)
{
    Vma* v = mmapFind(thread, faultAddr);
    if (!v)
        return false;
    if (forWrite && !(v->prot & mmapProtWrite))
        return false;
    if (!forWrite && !(v->prot & mmapProtRead))
        return false;

    uint64 pageAddr = pageAlignDown(faultAddr);
    if (pagingResolve(pageAddr) != 0)
        return true;

    uint64 frame = pmmAlloc();
    if (frame == 0)
        return false;
    memset((void*)frame, 0, frameSize);

    if (!v->anonymous) {
        FileDescriptor* fdEntry = &thread->fds[v->fd];
        if (fdEntry->kind == fdFree) {
            pmmFree(frame);
            return false;
        }

        uint64 savedOffset = fdEntry->offset;
        fdEntry->offset = v->fileOffset + (pageAddr - v->start);
        vfsRead(v->fd, (void*)frame, frameSize);
        fdEntry->offset = savedOffset;
    }

    uint64 flags = pageUser | ((v->prot & mmapProtWrite) ? pageWrite : 0);
    if (!(v->prot & mmapProtExec))
        flags |= pageNoExecute;
    if (!pagingMap(pageAddr, frame, flags)) {
        pmmFree(frame);
        return false;
    }
    return true;
}

void mmapReleaseAll(struct Thread* thread)
{
    memset(thread->vmas, 0, sizeof(thread->vmas));
}
