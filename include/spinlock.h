#ifndef FROSTBYTE_SPINLOCK_H
#define FROSTBYTE_SPINLOCK_H

#include "types.h"

typedef struct SpinLock {
    volatile uint32 locked;
} SpinLock;

void spinLockInit(SpinLock* lock);

uint64 spinLockAcquire(SpinLock* lock);

bool spinLockTryAcquire(SpinLock* lock, uint64* savedFlags);

void spinLockRelease(SpinLock* lock, uint64 flags);

bool spinLockIsLocked(const SpinLock* lock);

#endif
