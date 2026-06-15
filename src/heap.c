#include "heap.h"
#include "string.h"

typedef struct Block {
    usize size;
    struct Block* prev;
    struct Block* next;
    uint64 free;
} Block;

static uint64 heapBase;
static uint64 heapLimit;
static Block* head;

static uint64 alignUp(uint64 value, uint64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void heapInit(uint64 start, uint64 size)
{
    heapBase = alignUp(start, 16);
    heapLimit = start + size;

    head = (Block*)heapBase;
    head->size = (heapLimit - heapBase) - sizeof(Block);
    head->prev = nullptr;
    head->next = nullptr;
    head->free = 1;
}

static void split(Block* block, usize wanted)
{
    if (block->size < wanted + sizeof(Block) + 16)
        return;

    Block* rest = (Block*)((uint8*)block + sizeof(Block) + wanted);
    rest->size = block->size - wanted - sizeof(Block);
    rest->free = 1;
    rest->prev = block;
    rest->next = block->next;
    if (block->next)
        block->next->prev = rest;
    block->next = rest;
    block->size = wanted;
}

void* kmalloc(usize size)
{
    if (size == 0)
        size = 16;
    usize wanted = alignUp(size, 16);

    for (Block* block = head; block; block = block->next) {
        if (block->free && block->size >= wanted) {
            split(block, wanted);
            block->free = 0;
            return (uint8*)block + sizeof(Block);
        }
    }
    return nullptr;
}

void kfree(void* pointer)
{
    if (!pointer)
        return;

    Block* block = (Block*)((uint8*)pointer - sizeof(Block));
    block->free = 1;

    if (block->next && block->next->free) {
        Block* victim = block->next;
        block->size += sizeof(Block) + victim->size;
        block->next = victim->next;
        if (victim->next)
            victim->next->prev = block;
    }

    if (block->prev && block->prev->free) {
        Block* keep = block->prev;
        keep->size += sizeof(Block) + block->size;
        keep->next = block->next;
        if (block->next)
            block->next->prev = keep;
    }
}

void* krealloc(void* pointer, usize size)
{
    if (!pointer)
        return kmalloc(size);
    if (size == 0) {
        kfree(pointer);
        return nullptr;
    }

    Block* block = (Block*)((uint8*)pointer - sizeof(Block));
    usize wanted = alignUp(size, 16);
    if (block->size >= wanted)
        return pointer;

    void* fresh = kmalloc(size);
    if (!fresh)
        return nullptr;
    memcpy(fresh, pointer, block->size);
    kfree(pointer);
    return fresh;
}

uint64 heapFreeBytes(void)
{
    uint64 total = 0;
    for (Block* block = head; block; block = block->next)
        if (block->free)
            total += block->size;
    return total;
}

uint64 heapLargestFree(void)
{
    uint64 largest = 0;
    for (Block* block = head; block; block = block->next)
        if (block->free && block->size > largest)
            largest = block->size;
    return largest;
}

uint64 heapBlockCount(void)
{
    uint64 count = 0;
    for (Block* block = head; block; block = block->next)
        count++;
    return count;
}

uint64 heapTotal(void)
{
    return heapLimit - heapBase;
}

uint64 heapUsed(void)
{
    return heapTotal() - heapFreeBytes();
}
