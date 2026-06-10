#include "multiboot2.h"

static uint64 alignUp(uint64 value, uint64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

bool multibootFindModule(uint64 multibootInfo, uint64* start, uint64* end)
{
    uint32 totalSize = *(uint32*)multibootInfo;
    uint64 tagAddr = multibootInfo + 8;
    uint64 endAddr = multibootInfo + totalSize;

    while (tagAddr < endAddr) {
        MultibootTag* tag = (MultibootTag*)tagAddr;
        if (tag->type == multibootTagTypeEnd)
            break;
        if (tag->type == multibootTagTypeModule) {
            MultibootTagModule* module = (MultibootTagModule*)tag;
            *start = module->moduleStart;
            *end = module->moduleEnd;
            return true;
        }
        tagAddr = alignUp(tagAddr + tag->size, 8);
    }

    return false;
}
