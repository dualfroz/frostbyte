#include "ring.h"

void ringInit(RingBuffer* r, uint8* backingBuffer, usize capacity)
{
    r->data = backingBuffer;
    r->capacity = capacity;
    r->head = 0;
    r->count = 0;
}

void ringClear(RingBuffer* r)
{
    r->head = 0;
    r->count = 0;
}

bool ringIsEmpty(const RingBuffer* r) { return r->count == 0; }
bool ringIsFull(const RingBuffer* r) { return r->count == r->capacity; }
usize ringCount(const RingBuffer* r) { return r->count; }
usize ringCapacity(const RingBuffer* r) { return r->capacity; }
usize ringFreeSpace(const RingBuffer* r) { return r->capacity - r->count; }

bool ringPush(RingBuffer* r, uint8 byte)
{
    if (ringIsFull(r))
        return false;
    usize tail = (r->head + r->count) % r->capacity;
    r->data[tail] = byte;
    r->count++;
    return true;
}

bool ringPop(RingBuffer* r, uint8* out)
{
    if (ringIsEmpty(r))
        return false;
    if (out)
        *out = r->data[r->head];
    r->head = (r->head + 1) % r->capacity;
    r->count--;
    return true;
}

usize ringWrite(RingBuffer* r, const uint8* data, usize len)
{
    usize written = 0;
    while (written < len && ringPush(r, data[written]))
        written++;
    return written;
}

usize ringRead(RingBuffer* r, uint8* data, usize len)
{
    usize readCount = 0;
    while (readCount < len && ringPop(r, &data[readCount]))
        readCount++;
    return readCount;
}
