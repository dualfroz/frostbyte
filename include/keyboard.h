#ifndef FROSTBYTE_KEYBOARD_H
#define FROSTBYTE_KEYBOARD_H

#include "types.h"

void keyboardInit(void);

/* The real PS/2 IRQ1 handler (isr.c): reads the scancode off the hardware
   port and hands it to keyboardHandleScancode below. */
void keyboardHandler(void);

/*
 * Per-reader console input queues: a fixed table of ring-buffer-backed byte
 * queues (ring.h), each independently allocated and each fed a copy of
 * every decoded keystroke by keyboardHandleScancode -- like several
 * terminals plugged into one keyboard, every open reader sees every key.
 * This replaces an earlier design with exactly one global buffer shared by
 * every consumer (the kernel shell and every ring-3 process's fd 0 alike),
 * which had two real problems: concurrent readers raced directly on the
 * same head/tail indices with no locking, and its blocking wait
 * (keyboardReadChar's raw "sti; hlt" loop) was invisible to the scheduler --
 * a thread parked there could never be woken by schedWakeChannel, never
 * charged CFS-lite vruntime while "running" it, and never recovered by
 * threadKill the way every other blocking read/write in this kernel can be
 * (pipe.h's pipeReadBlocking/pipeWriteBlocking, vfs.h's fdFlagBlocking
 * pipe/fifo ends). Every reader here uses the exact same
 * schedBlockCurrent/schedWakeChannel wait-channel contract as pipe.c, wired
 * into vfs.c's fdConsole read path (vfsBlockingChannel) so syscall.c's
 * sysRead restart-syscall trick works on fd 0 exactly like it already does
 * on a pipe end.
 */
/* Bumped from 48 (this pool's very first, never-since-revisited size, set
   when sched.h's maxThreads was still 32): vfs.c's vfsFdInit grabs one
   reader automatically for EVERY thread ever created (kernel or user alike,
   whether it ever reads fd 0 or not, its own doc comment already says so)
   at CREATION time, which happens sequentially and entirely inside kmain()
   for every boot-time thread, well before schedStart() ever runs -- so the
   first 48 creation-order calls always succeeded and every one after them
   always silently got consoleReaderInvalid, deterministically, regardless
   of scheduling. shell.c's shellRun makes a SEPARATE, later, explicit
   keyboardOpenReader() call once the shell thread actually gets a
   timeslice (its own real input queue, distinct from its automatic-but-
   unused fd 0 slot) -- that call can only succeed if some earlier thread
   has ALREADY died and been reaped (reclaimOpenFds, sched.c) by then,
   freeing a slot back up. maxThreads grew from 32 to 168 across dozens of
   falas' own headroom bumps (sched.c's own history comment) while this
   completely separate pool never grew to match, so by FALA 33 kmain.c was
   creating far more than 48 threads before schedStart() ever ran --
   shellRun's own call was surviving purely on the timing-dependent hope
   that at least one of the first 48 automatic grabs belonged to an
   already-exited thread by the time it ran, not on any real guarantee.
   Adding one more automatically-grabbing kernel thread (hereDriverThread,
   kmain.c) tipped this from "usually still works" to "shell has no input"
   on a clean run -- caught live by this fala's own gate, not a review.
   Sized to maxThreads (168) plus the same +8 headroom margin sched.c's own
   bumps use, which is enough for every one of maxThreads' automatic
   grabs to always succeed deterministically PLUS shellRun's own extra
   explicit call on top, eliminating the race entirely rather than
   continuing to depend on exit timing. Bumped again (176 -> 184) in lockstep
   with sched.c's own maxThreads bump (168 -> 176, hcat/hsink/
   hereStringPipelineDriverThread) -- the same fixed maxThreads+8 relationship,
   not re-derived from scratch each time. Bumped again (184 -> 192) in
   lockstep with sched.c's own maxThreads bump (176 -> 184, pipecat/
   threeStagePipelineDriverThread), same relationship. Bumped again
   (192 -> 200) in lockstep with sched.c's own maxThreads bump (184 -> 192,
   cwdtest/cwdtarget), same relationship. Bumped again (200 -> 208) in
   lockstep with sched.c's own maxThreads bump (192 -> 200, envprint/
   envDriverThread), same relationship. Bumped again (208 -> 216) in
   lockstep with sched.c's own maxThreads bump (200 -> 208, nicetest/
   nicetarget), same relationship. Bumped again (216 -> 224) in lockstep
   with sched.c's own maxThreads bump (208 -> 216, date), same
   relationship. Bumped again (224 -> 232) in lockstep with sched.c's own
   maxThreads bump (216 -> 224, timetest), same relationship. Bumped again
   (232 -> 240) in lockstep with sched.c's own maxThreads bump (224 -> 232,
   lsDriverThread), same relationship. Bumped again (240 -> 248) in
   lockstep with sched.c's own maxThreads bump (232 -> 240,
   andOrDriverThread), same relationship. Bumped again (248 -> 256) in
   lockstep with sched.c's own maxThreads bump (240 -> 248, rmtest), same
   relationship. Bumped again (256 -> 264) in lockstep with sched.c's own
   maxThreads bump (248 -> 256, permtest plus its one transient runtime-
   spawned child), same relationship. Bumped again (264 -> 272) in lockstep
   with sched.c's own maxThreads bump (256 -> 264, dirpermtest), same
   relationship. Bumped again (272 -> 280) in lockstep with sched.c's own
   maxThreads bump (264 -> 272, grouptest plus its two transient runtime-
   spawned children), same relationship. Bumped again (280 -> 288) in
   lockstep with sched.c's own maxThreads bump (272 -> 280, pidfdtest plus
   its one transient runtime-spawned killvictim), same relationship.
   Bumped again (288 -> 296) in lockstep with sched.c's own maxThreads bump
   (280 -> 288, mmaptest plus its one transient runtime-spawned
   mmapprotvictim), same relationship. */
#define maxConsoleReaders 296
#define consoleReaderInvalid (-1)

/* Allocates a fresh, empty reader queue and returns its handle, or
   consoleReaderInvalid if the table is full. Every thread gets one for its
   own fd 0 at creation time (vfs.c's vfsFdInit) -- one keystroke delivered
   by keyboardHandleScancode reaches every thread's queue independently, so
   two unrelated processes both reading fd 0 each see their own copy rather
   than racing for the same byte. */
int keyboardOpenReader(void);

/* Releases a reader's slot for reuse. No-op on an invalid handle. */
void keyboardCloseReader(int handle);

/* Non-blocking: pops up to length queued bytes into buffer, in the order
   they were typed. Returns the number of bytes actually copied (which may
   be less than length, or 0 only when length itself was 0), or -1 for an
   invalid/closed handle or when the queue is currently empty (a live
   keyboard has no end-of-file the way a pipe's write end closing does --
   "nothing typed yet" and "try again later" are the same thing). */
int64 keyboardReaderRead(int handle, void* buffer, uint64 length);

/* Blocks (parks the calling thread and halts) until at least one byte is
   available, then behaves exactly like keyboardReaderRead. Only valid from
   plain kernel-thread context (see shell.c) -- calling this from inside a
   syscall's interrupt-gate handler would freeze the whole machine, not just
   the caller, the same hazard pipeReadBlocking's doc comment (pipe.h)
   describes; a syscall handler must instead use vfsBlockingChannel (vfs.h)
   with the restart-syscall pattern. Returns -1 immediately, without
   blocking, for an invalid handle. */
int64 keyboardReaderReadBlocking(int handle, void* buffer, uint64 length);

/* The address schedBlockCurrent/schedWakeChannel use for this reader's wait
   queue -- mirrors pipe.h's pipeChannel exactly, including returning
   nullptr for an invalid handle so a syscall handler can tell "do not
   block, this fd cannot ever produce more data" apart from "wait and
   retry". */
const void* keyboardReaderChannel(int handle);

/* Non-consuming readiness peek for poll.h's vfsPollReady (vfs.c): true iff
   keyboardReaderRead would return actual bytes right now, without popping
   any of them off the ring. False for an invalid handle. */
bool keyboardReaderHasData(int handle);

/* The scancode-decoding half of keyboardHandler, split out so tests (and
   the live boot demo in kmain.c, which has no automated way to drive a real
   PS/2 device under headless QEMU) can inject a keystroke exactly the way
   real hardware would, without needing to fake port I/O. Handles shift and
   (now) left-Ctrl make/break codes, intercepts the real tty INTR/SUSP
   combination (Ctrl-C/Ctrl-Z) as job-control signals to the foreground
   process group instead of ordinary data (see the file comment in
   keyboard.c), and otherwise broadcasts the decoded character to every open
   reader queue, waking anyone blocked on one via schedWakeChannel. */
void keyboardHandleScancode(uint8 scancode);

/* Real tcsetpgrp(3): names the process group (sched.h's Thread.pgid) that
   OWNS "the terminal" from this point on -- the only pgid keyboardHandleScancode's
   Ctrl-C/Ctrl-Z interception (below) ever delivers sigInt/sigTstp to.
   0 (the default, before anything ever calls this -- see keyboardInit) can
   never match any live user thread's own pgid (every boot-spawned process
   becomes its own new group leader, sched.c's initialPgid, so the lowest
   possible real pgid is 1; only the permanent idle thread, which never
   reads a keyboard, is ever pgid 0), so "nobody has claimed the foreground
   yet" and "deliver to no one" are naturally the same state, with no extra
   sentinel needed. */
void keyboardSetForegroundPgid(uint32 pgid);

/* The pgid keyboardSetForegroundPgid last recorded (0 if never called since
   keyboardInit) -- real tcgetpgrp(3)'s counterpart, exposed to userspace
   through sysGetFgPgrp/userGetForegroundPgid. */
uint32 keyboardForegroundPgid(void);

#endif
