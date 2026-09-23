#include "pmm.h"
#include "multiboot2.h"
#include "string.h"
#include "ports.h"

extern uint8 kernelPhysEnd[];

static uint8* bitmap;
static uint64 frameCount;
static uint64 usedFrames;
static uint64 totalMemoryBytes;
static uint64 reservedEnd;
static uint64 allocHint;

static void markUsed(uint64 frame)
{
    if (frame >= frameCount)
        return;
    uint64 index = frame / 8;
    uint8 bit = (uint8)(1 << (frame % 8));
    if (!(bitmap[index] & bit)) {
        bitmap[index] |= bit;
        usedFrames++;
    }
}

static void markFree(uint64 frame)
{
    if (frame >= frameCount)
        return;
    uint64 index = frame / 8;
    uint8 bit = (uint8)(1 << (frame % 8));
    if (bitmap[index] & bit) {
        bitmap[index] &= (uint8)~bit;
        usedFrames--;
    }
}

static bool isUsed(uint64 frame)
{
    return (bitmap[frame / 8] & (1 << (frame % 8))) != 0;
}

static uint64 alignUp(uint64 value, uint64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void pmmReserve(uint64 start, uint64 size)
{
    uint64 first = start / frameSize;
    uint64 last = (start + size + frameSize - 1) / frameSize;
    for (uint64 frame = first; frame < last; frame++)
        markUsed(frame);
}

uint64 pmmReservedEnd(void)
{
    return reservedEnd;
}

void pmmInit(uint64 multibootInfo, uint64 protectedEnd)
{
    uint32 totalSize = *(uint32*)multibootInfo;
    uint64 tagAddr = multibootInfo + 8;
    uint64 endAddr = multibootInfo + totalSize;

    MultibootTagMmap* mmap = nullptr;
    uint64 highestAddr = 0;

    while (tagAddr < endAddr) {
        MultibootTag* tag = (MultibootTag*)tagAddr;
        if (tag->type == multibootTagTypeEnd)
            break;
        if (tag->type == multibootTagTypeMmap)
            mmap = (MultibootTagMmap*)tag;
        tagAddr = alignUp(tagAddr + tag->size, 8);
    }

    if (mmap) {
        uint64 entryAddr = (uint64)mmap->entries;
        uint64 mmapEnd = (uint64)mmap + mmap->size;
        while (entryAddr < mmapEnd) {
            MultibootMmapEntry* entry = (MultibootMmapEntry*)entryAddr;
            if (entry->type == multibootMemoryAvailable) {
                uint64 regionEnd = entry->baseAddr + entry->length;
                totalMemoryBytes += entry->length;
                if (regionEnd > highestAddr)
                    highestAddr = regionEnd;
            }
            entryAddr += mmap->entrySize;
        }
    }

    if (highestAddr == 0)
        highestAddr = 128 * 1024 * 1024;

    frameCount = highestAddr / frameSize;

    uint64 bitmapAnchor = (uint64)kernelPhysEnd;
    if (protectedEnd > bitmapAnchor)
        bitmapAnchor = protectedEnd;
    if (endAddr > bitmapAnchor)
        bitmapAnchor = endAddr;
    bitmap = (uint8*)alignUp(bitmapAnchor, frameSize);
    uint64 bitmapBytes = (frameCount + 7) / 8;
    memset(bitmap, 0xFF, bitmapBytes);
    usedFrames = frameCount;

    if (mmap) {
        uint64 entryAddr = (uint64)mmap->entries;
        uint64 mmapEnd = (uint64)mmap + mmap->size;
        while (entryAddr < mmapEnd) {
            MultibootMmapEntry* entry = (MultibootMmapEntry*)entryAddr;
            if (entry->type == multibootMemoryAvailable) {
                uint64 first = alignUp(entry->baseAddr, frameSize) / frameSize;
                uint64 last = (entry->baseAddr + entry->length) / frameSize;
                for (uint64 frame = first; frame < last; frame++)
                    markFree(frame);
            }
            entryAddr += mmap->entrySize;
        }
    }

    reservedEnd = alignUp((uint64)bitmap + bitmapBytes, frameSize);
    pmmReserve(0, reservedEnd);
    allocHint = reservedEnd / frameSize;
}

static uint64 allocFrame(void)
{
    for (uint64 offset = 0; offset < frameCount; offset++) {
        uint64 frame = allocHint + offset;
        if (frame >= frameCount)
            frame -= frameCount;
        if (!isUsed(frame)) {
            markUsed(frame);
            allocHint = frame + 1;
            return frame * frameSize;
        }
    }
    return 0;
}

uint64 pmmAlloc(void)
{
    uint64 flags = saveAndDisableInterrupts();
    uint64 frame = allocFrame();
    restoreInterrupts(flags);
    return frame;
}

void pmmFree(uint64 frame)
{
    uint64 flags = saveAndDisableInterrupts();
    markFree(frame / frameSize);
    restoreInterrupts(flags);
}

uint64 pmmTotalFrames(void)
{
    return frameCount;
}

uint64 pmmUsedFrames(void)
{
    return usedFrames;
}

uint64 pmmFreeFrames(void)
{
    return frameCount - usedFrames;
}

uint64 pmmTotalBytes(void)
{
    return totalMemoryBytes;
}
