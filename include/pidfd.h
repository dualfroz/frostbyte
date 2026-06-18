#ifndef FROSTBYTE_PIDFD_H
#define FROSTBYTE_PIDFD_H

#include "types.h"

#define maxPidfds 32

/*
 * Real Linux precedent: pidfd_open(2) (Linux 5.3+) exists for exactly the
 * ABA-style race sched.c's own allocSlot doc comment describes and
 * deliberately leaves unfixed for plain pid arguments -- killtest.c's own
 * comment already admits it ("killing it again must fail cleanly, not
 * resurrect a slot that has since been reused") without ever guarding
 * against it. A raw numeric pid can always be reassigned to a completely
 * unrelated process once its original holder is reaped, so a pid cached
 * from an earlier spawn/lookup is fundamentally unsafe to act on later
 * with no way to tell "still the same process" apart from "coincidentally
 * the same number now". Real Linux's own fix was not to make pid itself
 * opaque -- every existing kill(2)/waitpid(2)/setpgid(2) caller still
 * takes a raw pid, completely unchanged, forever -- but to add a SEPARATE,
 * opt-in handle type that pins one specific incarnation of a slot instead.
 * This file mirrors that shape exactly: pidfdOpen captures the target
 * thread's CURRENT sched.c Thread.generation (the same "distinguish this
 * specific occupant from any future occupant of the same numeric slot"
 * idea pipe.c's own pipeIncarnation already uses for pipe handles), and
 * every later pidfd operation re-checks it before ever touching the
 * target -- if the original thread has since been reaped (its generation
 * reset to 0 by threadReap/threadDiscard's memset) or the slot has been
 * handed to a brand new, unrelated thread (a different, fresh nonzero
 * generation), the pidfd cleanly reports "gone" (threadInvalidId) instead
 * of silently operating on whichever live thread now happens to occupy
 * that slot number. sched.c's raw pid-based threadKill/threadJoin/
 * sigRaiseInfo/threadSetPgid/threadSetPriority are deliberately untouched
 * by this file -- see allocSlot's own doc comment (sched.c) for why that
 * remains a separate, much larger, deferred topic.
 */

/* Opens a new pidfd handle on thread `targetId`, pinning its CURRENT
   generation (sched.h). Returns a small non-negative handle for
   pidfdTarget/pidfdClose below, or -1 for id 0 (idle, never a valid
   target), an out-of-range id, an already-unused slot, or a full pidfd
   table (maxPidfds handles system-wide, mirroring pipe.c's own fixed-size
   maxPipes table). Does not itself keep the target alive or pinned in any
   way -- it can still exit, be killed, and be reaped by anyone (this
   kernel's pids are never ownership-scoped, see sched.h's parentId doc
   comment) while a pidfd on it is open; that is exactly the case
   pidfdTarget below exists to detect. */
int pidfdOpen(uint32 targetId);

/* Returns the live target thread id this handle still safely refers to,
   or threadInvalidId if the handle itself is invalid/closed OR the
   original target has been reaped or recycled since pidfdOpen -- the one
   check every pidfd-based operation needs, folded into a single call so
   no caller can accidentally act on a stale target by forgetting a
   separate validity check. A target that has exited but not yet been
   reaped (threadDead) still returns its real id here -- exactly like a
   raw pid, waiting on it should still be able to observe the real exit
   status once, via whatever reaps it. */
uint32 pidfdTarget(int handle);

/* Frees a pidfd handle. No-op on an already-invalid handle. Never affects
   the target thread itself in any way -- a pidfd, like a real Linux one,
   is purely an observer/signaler, not a reference that keeps anything
   alive. */
void pidfdClose(int handle);

#endif
