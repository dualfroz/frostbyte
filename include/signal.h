#ifndef FROSTBYTE_SIGNAL_H
#define FROSTBYTE_SIGNAL_H

#include "types.h"
#include "isr.h"
#include "sched.h"
#include "signalnum.h"

/*
 * Real (catchable) signal delivery on top of the single-address-space,
 * ring0/ring3 kernel: a thread can install a handler for a signal number,
 * mask/unmask signals, and have another thread (or itself) raise one --
 * delivery happens the moment the target thread next returns to ring3
 * through the interrupt-return path (isr.c's deliverPendingSignals), by
 * rewriting its saved trapframe to enter a small shared trampoline page
 * (sigInstallTrampoline below) instead of resuming where it was
 * interrupted. The trampoline calls the handler, then traps back in via
 * sysSigReturn, which restores the exact pre-signal register snapshot this
 * kernel keeps in the Thread struct itself (sigSavedRegs) -- no signal
 * frame is ever pushed onto the user stack, since there is no need to: the
 * saved context already lives in kernel memory, unlike a real kernel with
 * per-process address spaces where the handler must run in a stack the
 * kernel does not otherwise own a copy of.
 *
 * Job control (SIGSTOP/SIGCONT/SIGTSTP) is real, on top of a genuine
 * threadStopped ThreadState (sched.h): SIGSTOP, like SIGKILL, bypasses the
 * pending/blocked/handler machinery entirely -- sigRaise delegates it
 * straight to threadStop (sched.c), so it cannot be caught, blocked, or
 * ignored, matching real Unix. SIGCONT is NOT bypassed the same way: it
 * still sets its pending bit and runs through the normal scan (so an
 * installed handler for it genuinely fires), but sigRaise ALSO
 * unconditionally calls threadContinue as a side effect, independent of
 * sigBlocked -- the resume itself is not something a handler or a mask can
 * defer, only whether a handler additionally runs is. SIGTSTP is the
 * catchable half of job control: its real-world default disposition is
 * Stop rather than Terminate (sigDefaultIsStop), reported as
 * sigOutcomeStop for isr.c to act on the same way it already acts on
 * sigOutcomeTerminate, but -- unlike SIGSTOP -- an installed handler for it
 * runs instead of stopping, and it can be blocked/ignored like any other
 * catchable signal.
 *
 * Per-signal sa_mask (POSIX sigaction's sa_mask, not sigprocmask's whole-
 * process mask): sigSetHandler takes a `mask` alongside the handler address,
 * stored in Thread.sigHandlerMasks[signum] (sched.h) -- while that signum's
 * handler is running, sigBlocked is the union of whatever was already
 * blocked, this mask, and (unless sigFlagNoDefer -- signalnum.h -- is set
 * for it) the signal itself, self-blocked like real Unix's implicit
 * default. A DIFFERENT, unmasked signal arriving while a handler runs is
 * therefore genuinely deliverable, not blanket-blocked the way an earlier
 * version of this file (see git history) used to: sigConsumePending is
 * gated by Thread.sigHandlerDepth against sigMaxNestDepth (sched.h), not by
 * a single "one handler at a time" bool, so a second handler can run
 * NESTED inside the first (its own context pushed onto the small
 * sigSavedRegs/sigSavedBlocked stack) and the first resumes, via
 * sigOnSigReturn popping that same stack, exactly where it left off once
 * the nested one returns. user/nesttarget.c is the live ring-3 proof for
 * two DIFFERENT signals; user/nodefertest.c is the same mechanism proving
 * the SAME signal number can nest into its own still-active handler when
 * registered with sigFlagNoDefer (and, by contrast, genuinely cannot
 * without it); sigTest.c drives the stack directly for fully deterministic
 * versions of both.
 *
 * Early wake of a Sleeping/Blocked target (real Unix's interruptible
 * sleep/EINTR): sigRaise calls sched.c's threadWakeForSignal for any
 * signal signal.c's sigWouldDeliver determines would genuinely be
 * delivered (not blocked, not ignored) -- forcing the target straight to
 * threadRunnable so the NEXT schedule() eventually resumes it, reaching a
 * return-to-ring3 point (isr.c's deliverPendingSignals) it might otherwise
 * NEVER naturally reach again (e.g. user/eintrtarget.c's read, which no
 * writer will ever satisfy). A blocked or ignored signal leaves a
 * Sleeping/Blocked target's wait completely undisturbed, exactly like real
 * Unix -- sigWouldDeliver is checked first. threadWakeForSignal itself
 * excludes threadStopped on purpose: job control's "stopped means stopped"
 * must survive an ordinary signal arriving too, so only threadContinue (an
 * explicit SIGCONT) may ever move a Stopped thread, the same asymmetry
 * schedWakeChannel already has for the same reason.
 *
 * SA_RESTART / EINTR (signalnum.h's sigFlagRestart, sched.h's
 * Thread.sigInRetryableSyscall): being woken early (or simply signaled
 * while already Blocked) answers "does the target wake up at all", not
 * "what does the interrupted syscall return once its handler is done" --
 * that second question is what sa_flags settles. Before this existed,
 * every retryable syscall (sysRead/sysWrite/sysWait, syscall.c) restarted
 * unconditionally once a handler returned, since sysSigReturn always
 * restores the exact rewound rip sigConsumePending captured. Now
 * sigConsumePending checks, at the moment it is about to push a custom
 * handler's context, whether the thread is genuinely mid one of those three
 * (sigInRetryableSyscall) and the signal being delivered lacks
 * sigFlagRestart for its own handler; if so, it rewrites the ABOUT-TO-BE-
 * SAVED context in place (rip past the interrupted `int $0x80`, rax to
 * errInterrupted) before pushing it, so sigOnSigReturn's later, byte-for-
 * byte-faithful restore resumes the caller having genuinely observed an
 * interrupted syscall, not a silently retried one -- real EINTR, not an
 * approximation of it. user/restarttarget.c is the live ring-3 proof of
 * both halves (with and without the flag); sigTest.c drives
 * sigConsumePending directly for the deterministic one.
 *
 * A thread self-targeting SIGSTOP through sysSigSend (syscall.c) is
 * reasoned about and exercised by
 * schedTest.c's state-machine coverage of threadStop accepting
 * threadRunning, but -- unlike the external-stop/continue scenario
 * user/jobtest.c demonstrates live -- has no live ring-3 demo of its own
 * this session: proving it would need a second, independent process to
 * race-freely learn "the target has now actually stopped" before sending
 * SIGCONT, which is exactly the kind of guess-free rendezvous this file's
 * OWN sigtarget.c comment already had to solve once for a different race,
 * and doing it again here was judged not worth the added live-boot
 * surface for a self-stop path whose actual mechanism (threadStop, called
 * on the current thread) is not otherwise unusual. SIGKILL is intentionally
 * NOT routed through this pending/blocked machinery at all: sigRaise
 * delegates it straight to the existing threadKill (sched.c), the one
 * unconditional, unmaskable, uncatchable termination path this kernel
 * already had (now also accepting threadStopped, since a real
 * kill(pid, SIGKILL) kills a stopped process too) -- exactly like
 * real kill(pid, SIGKILL) never goes through a handler either.
 */

/* Signal numbers, handler sentinels, sigSetFailed and the sigMask* "how"
   values all live in signalnum.h, shared verbatim with userspace (see that
   header for why it is split out). */

/* sigMaxSignals (one bit per signal number in Thread.sigPending/
   sigBlocked, and the size of Thread.sigHandlers[]) is defined in sched.h,
   next to the Thread struct that actually needs it as an array bound --
   signal.h cannot define it itself without sched.h including signal.h
   right back, which would be circular. */

/* Every signal bit set -- a plain "block literally everything" convenience
   constant for callers/tests that want to exercise sigProcMask's own
   sigKill/sigStop stripping, or job control's "resume happens regardless of
   sigBlocked" guarantee, with the widest possible mask. Not used by
   sigConsumePending's own handler-entry logic any more (see the file
   comment's per-signal sa_mask paragraph) -- that is real, per-handler
   Thread.sigHandlerMasks[signum] now, not this one blanket value. */
#define sigAllBlocked (~0ull)

/* Fixed virtual address of the one shared, read+execute, never-write,
   user-accessible trampoline page every process's signal handlers are
   entered through (sigInstallTrampoline maps it once at boot). Chosen
   deliberately far from both the boot-time 1 GiB identity-mapped range
   ([0, 0x40000000), see boot.S's bootPd -- pagingMap refuses to touch
   anything inside a huge-page-mapped range anyway) and the per-binary
   elfSlotSize=0x10000000 slots USER_BASE hands out one at a time starting
   at 0x40000000 (syscall.c's userSpaceMin) -- at the current growth rate
   (Makefile) that sequence would need roughly three thousand more user
   binaries to ever reach here, and every process already shares this one
   address space regardless (see the architecture note in sched.h), so one
   fixed shared page for ALL of them, like a real kernel's vDSO, is exactly
   as much mapping as this needs. */
#define sigTrampolineVirtualAddr 0x0000700000000000ull

