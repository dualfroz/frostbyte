#ifndef FROSTBYTE_SPINLOCK_H
#define FROSTBYTE_SPINLOCK_H

#include "types.h"

/*
 * SpinLock: an interrupt-safe mutual-exclusion lock for state shared between
 * thread context and interrupt handlers (irq0/irq1 currently run with
 * interrupts re-enabled per pic.c, so a naive "just disable interrupts"
 * critical section is not enough once two handlers can nest).
 *
 * Acquire disables interrupts and spins on an atomic test-and-set before
 * returning; release restores whatever the interrupt flag was at the
 * matching acquire. This is correct and SMP-ready even though frostbyte is
 * currently single-core: on one core, acquiring an already-held lock is a
 * bug (it would spin forever with interrupts off), so spinLockTryAcquire is
 * provided for call sites -- and tests -- that need to observe contention
 * without deadlocking.
 */

typedef struct SpinLock {
    volatile uint32 locked;
} SpinLock;

void spinLockInit(SpinLock* lock);

/* Disables interrupts, then spins until the lock is acquired. Returns the
   interrupt flag as it was immediately before this call, to be passed back
   to spinLockRelease unchanged (callers must not interleave two different
   locks' flags). */
uint64 spinLockAcquire(SpinLock* lock);

/* Non-blocking. On success (true), interrupts are disabled and *savedFlags
   holds the pre-call flag word, exactly as spinLockAcquire would leave
   things. On failure (false) nothing is touched -- interrupts are left
   exactly as the caller had them. */
bool spinLockTryAcquire(SpinLock* lock, uint64* savedFlags);

/* flags must be the value spinLockAcquire/spinLockTryAcquire handed back. */
void spinLockRelease(SpinLock* lock, uint64 flags);

bool spinLockIsLocked(const SpinLock* lock);

#endif
