#include "spinlock.h"
#include "ports.h"

void spinLockInit(SpinLock* lock)
{
    lock->locked = 0;
}

uint64 spinLockAcquire(SpinLock* lock)
{
    uint64 flags = saveAndDisableInterrupts();
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE) != 0) {
    }
    return flags;
}

bool spinLockTryAcquire(SpinLock* lock, uint64* savedFlags)
{
    uint64 flags = saveAndDisableInterrupts();
    if (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE) != 0) {
        restoreInterrupts(flags);
        return false;
    }
    *savedFlags = flags;
    return true;
}

void spinLockRelease(SpinLock* lock, uint64 flags)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
    restoreInterrupts(flags);
}

bool spinLockIsLocked(const SpinLock* lock)
{
    return __atomic_load_n(&lock->locked, __ATOMIC_ACQUIRE) != 0;
}
