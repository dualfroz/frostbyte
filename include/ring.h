#ifndef FROSTBYTE_RING_H
#define FROSTBYTE_RING_H

#include "types.h"

typedef struct RingBuffer {
    uint8* data;
    usize capacity;
    usize head;
    usize count;
} RingBuffer;

void ringInit(RingBuffer* r, uint8* backingBuffer, usize capacity);
void ringClear(RingBuffer* r);

bool ringIsEmpty(const RingBuffer* r);
bool ringIsFull(const RingBuffer* r);
usize ringCount(const RingBuffer* r);
usize ringCapacity(const RingBuffer* r);
usize ringFreeSpace(const RingBuffer* r);

bool ringPush(RingBuffer* r, uint8 byte);
bool ringPop(RingBuffer* r, uint8* out);

usize ringWrite(RingBuffer* r, const uint8* data, usize len);
usize ringRead(RingBuffer* r, uint8* data, usize len);

#endif
