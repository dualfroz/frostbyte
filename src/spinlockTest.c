#include "ktest.h"
#include "spinlock.h"

KTEST(spinLockInitStartsUnlocked)
{
    SpinLock lock;
    spinLockInit(&lock);
    KT_FALSE(t, spinLockIsLocked(&lock));
}

KTEST(spinLockAcquireReleaseCycle)
{
    SpinLock lock;
    spinLockInit(&lock);

    uint64 flags = spinLockAcquire(&lock);
    KT_TRUE(t, spinLockIsLocked(&lock));

    spinLockRelease(&lock, flags);
    KT_FALSE(t, spinLockIsLocked(&lock));

    flags = spinLockAcquire(&lock);
    KT_TRUE(t, spinLockIsLocked(&lock));
    spinLockRelease(&lock, flags);
    KT_FALSE(t, spinLockIsLocked(&lock));
}

KTEST(spinLockTryAcquireFailsWhenHeld)
{
    SpinLock lock;
    spinLockInit(&lock);

    uint64 flags;
    KT_TRUE(t, spinLockTryAcquire(&lock, &flags));
    KT_TRUE(t, spinLockIsLocked(&lock));

    uint64 otherFlags = 0xDEADBEEF;
    KT_FALSE(t, spinLockTryAcquire(&lock, &otherFlags));
    KT_EQU(t, otherFlags, 0xDEADBEEF);
    KT_TRUE(t, spinLockIsLocked(&lock));

    spinLockRelease(&lock, flags);
    KT_FALSE(t, spinLockIsLocked(&lock));

    KT_TRUE(t, spinLockTryAcquire(&lock, &flags));
    spinLockRelease(&lock, flags);
}

KTEST(spinLockIndependentLocksDoNotInterfere)
{
    SpinLock a, b;
    spinLockInit(&a);
    spinLockInit(&b);

    uint64 flagsA = spinLockAcquire(&a);
    KT_TRUE(t, spinLockIsLocked(&a));
    KT_FALSE(t, spinLockIsLocked(&b));

    uint64 flagsB = spinLockAcquire(&b);
    KT_TRUE(t, spinLockIsLocked(&a));
    KT_TRUE(t, spinLockIsLocked(&b));

    spinLockRelease(&a, flagsA);
    KT_FALSE(t, spinLockIsLocked(&a));
    KT_TRUE(t, spinLockIsLocked(&b));

    spinLockRelease(&b, flagsB);
    KT_FALSE(t, spinLockIsLocked(&b));
}
