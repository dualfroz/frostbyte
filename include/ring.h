#ifndef FROSTBYTE_RING_H
#define FROSTBYTE_RING_H

#include "types.h"

/*
 * RingBuffer: a fixed-capacity byte circular buffer over a caller-supplied
 * backing array. No heap dependency, so it is safe to use for early
 * interrupt-driven queues (keyboard scancodes, serial RX) that need to
 * exist before the kernel heap is initialised.
 */

typedef struct RingBuffer {
    uint8* data;
    usize capacity;
    usize head;   /* next byte to read */
    usize count;  /* bytes currently queued */
} RingBuffer;

void ringInit(RingBuffer* r, uint8* backingBuffer, usize capacity);
void ringClear(RingBuffer* r);

bool ringIsEmpty(const RingBuffer* r);
bool ringIsFull(const RingBuffer* r);
usize ringCount(const RingBuffer* r);
usize ringCapacity(const RingBuffer* r);
usize ringFreeSpace(const RingBuffer* r);

/* Single-byte push/pop. Push returns false (buffer unchanged) when full;
   pop returns false (out untouched) when empty. */
bool ringPush(RingBuffer* r, uint8 byte);
bool ringPop(RingBuffer* r, uint8* out);

/* Bulk transfer. Writes/reads as many bytes as fit/are available (may be
   fewer than requested) and returns the count actually transferred. */
usize ringWrite(RingBuffer* r, const uint8* data, usize len);
usize ringRead(RingBuffer* r, uint8* data, usize len);

#endif
