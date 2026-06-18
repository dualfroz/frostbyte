#ifndef FROSTBYTE_PIPE_H
#define FROSTBYTE_PIPE_H

#include "types.h"

#define maxPipes 32
#define pipeBufferSize 256

/*
 * Anonymous pipes: a fixed table of ring-buffer-backed byte queues, each
 * shared between one read end and one write end. The raw pipeRead/pipeWrite
 * primitives are always non-blocking, matching real O_NONBLOCK semantics:
 *
 *   pipeRead:
 *     >0  bytes actually read
 *      0  end-of-file: buffer drained AND the write end is closed
 *     -1  no data right now but the write end is still open (retry later)
 *
 *   pipeWrite:
 *     >0  bytes actually written
 *      0  length was 0
 *     -1  broken pipe (no read end left), or no free space right now
 *         (retry later)
 *
 * pipeReadBlocking/pipeWriteBlocking are built on top: they park the calling
 * kernel thread on the pipe's wait channel (sched.h) instead of returning
 * "try again", and are woken as soon as the other end changes the condition
 * (data arrives, space frees up, or either end closes). They only make sense
 * from kernel-thread context (see the producer/consumer demo in kmain.c) -
 * a single process's own fd table cannot hand one pipe end to a second,
 * independent execution context by itself.
 *
 * pipeCreateDetached + pipeAddReader/pipeAddWriter (below) close that gap
 * without needing fork(): a handle from pipeCreateDetached starts with
 * NEITHER end attached (unlike pipeCreate, which assumes its caller
 * immediately owns both), so it can be handed to two entirely separate,
 * independently spawned processes -- typically as a decimal string in argv
 * (see user/fifomaster.c) -- each of which attaches its own end via
 * vfsPipeAttach (vfs.h). syscall.c's sysRead/sysWrite give a real blocking
 * read()/write() to whichever of those two ends is waiting, using the same
 * restart-the-syscall trick sysWait already uses for threadJoin.
 */

int pipeCreate(void);

/* Like pipeCreate, but neither end starts "attached": readers and writers
   both begin at 0, and pipeRead/pipeWrite treat "nobody has attached that
   end YET" as "try again" rather than the true EOF/broken-pipe result they
   report once an end that WAS attached goes away (see everHadReader/
   everHadWriter, pipe.c) -- otherwise a writer that attaches before any
   reader does would see an immediately, permanently "broken" pipe, and a
   reader that attaches first would see immediate spurious EOF. */
int pipeCreateDetached(void);

/* Registers a new, independent reader/writer on an existing (typically
   pipeCreateDetached'd) handle -- vfsPipeAttach (vfs.h) calls these when a
   process attaches its end. No-op on an invalid handle. */
void pipeAddReader(int handle);
void pipeAddWriter(int handle);

/* True if a write to this pipe can never succeed again: either the handle
   is not valid, or a reader was attached at some point and every attached
   reader has since gone away. False for a pipeCreateDetached handle that
   simply has not had its reader attach yet -- that is "try again", not
   broken. Lets syscall.c's sysWrite tell "keep retrying" apart from "give
   up and return -1" without reaching into pipe.c's own Pipe struct. */
bool pipeWriteBroken(int handle);

/* The address schedBlockCurrent/schedWakeChannel use for this pipe's wait
   queue -- the same one pipeReadBlocking/pipeWriteBlocking already block
   on. Exposed so a syscall handler (syscall.c) can park on it via the
   restart-the-syscall pattern instead of calling halt() directly (which,
   run from inside an interrupt-gate syscall handler with IF=0, would freeze
   the whole machine rather than just the calling thread -- see sysWait's
   own doc comment in syscall.c for the same hazard). Returns nullptr for
   an invalid handle.  */
const void* pipeChannel(int handle);

bool pipeValid(int handle);

/* Opaque token identifying THIS specific pipe living at `handle`'s slot,
   distinct from any other pipe that has occupied (or will ever occupy) the
   same numeric handle before or after -- this fixed-size table reuses a
   slot number the moment both its ends are gone (pipeCloseRead/
   pipeCloseWrite's pipeReleaseIfUnused), so pipeValid(handle) alone cannot
   tell a caller that cached a handle across such a release "the same pipe I
   bound to earlier" apart from "a different, unrelated pipe that happens to
   have been assigned the same number since" (see vfs.c's SFS-backed named
   fifo bindings, sfsFifoHandleFor, the motivating caller). Always non-zero
   for any handle pipeCreate/pipeCreateDetached has ever handed out; 0 for
   an invalid handle, which can never equal a real incarnation, so a naive
   "compare and treat 0 as impossible" check is always safe. */
uint32 pipeIncarnation(int handle);

int64 pipeRead(int handle, void* buffer, uint64 length);
int64 pipeWrite(int handle, const void* buffer, uint64 length);

/* Non-consuming readiness peeks for poll.h's vfsPollReady (vfs.c): report
   exactly the same two conditions pipeRead/pipeWrite's own success paths
   check (data queued or true EOF; free space in the ring), without moving
   a single byte or touching readers/writers bookkeeping. False for an
   invalid handle either way. */
bool pipeHasData(int handle);
bool pipeHasSpace(int handle);

/* Blocks (parks the calling thread and halts) until data is available or
   true end-of-file is reached. Returns exactly what pipeRead would once the
   wait is over; -1 only for an invalid handle (never blocks in that case). */
int64 pipeReadBlocking(int handle, void* buffer, uint64 length);

/* Blocks until free space is available. Returns exactly what pipeWrite would
   once the wait is over; returns -1 immediately without blocking for an
   invalid handle or a broken pipe (no reader left), since neither condition
   can ever resolve by waiting. */
int64 pipeWriteBlocking(int handle, const void* buffer, uint64 length);

void pipeCloseRead(int handle);
void pipeCloseWrite(int handle);

/* Frees a pipeCreateDetached handle that nobody has ever attached a reader
   or writer to (readers==0 and writers==0) -- the one case pipeCloseRead/
   pipeCloseWrite's own release-when-both-zero check (pipeReleaseIfUnused)
   can never reach on its own, since neither is ever called for an end that
   was never attached in the first place. Needed by a named pipe (vfs.h's
   vfsMkfifo/vfsUnlink) created but never opened by anyone: without this, its
   underlying handle would sit "used" in the fixed-size pipe table forever
   once the vfs node naming it is removed. No-op (and reports false) if the
   handle is invalid or still has a reader or writer attached -- unlinking a
   named pipe that IS currently open must leave it alone, matching real
   unlink-while-open semantics (existing fds keep working off the raw handle
   number, not the vfs node). */
bool pipeDestroyIfUnattached(int handle);

#endif
