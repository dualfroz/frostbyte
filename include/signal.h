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

/* Fixed virtual address of the shared, read-only (never write, never
   execute), user-accessible SigInfo (signalnum.h) data page(s) every
   process's sigFlagSigInfo handlers are handed a pointer into --
   sigInstallInfoPage maps it once at boot, right after the trampoline page
   above, in the same reserved high range for the same reasons that page's
   own comment already gives (nowhere near either the boot-time identity
   map or any real per-binary elfSlotSize slot). One frameSize-sized page
   holds frameSize/sizeof(SigInfo) slots; sigInstallInfoPage maps as many
   consecutive pages, starting here, as threadCapacity()*sigMaxNestDepth
   slots actually need (today: 96*4 = 384 slots, comfortably inside 2
   pages) -- see sigConsumePending for how a (thread id, handler-nesting
   depth) pair maps to one specific slot, the same indexing scheme
   sigSavedRegs/sigSavedBlocked (sched.h) already use for the exact same
   pair, just kept in a user-readable page here instead of Thread itself
   since a handler running in ring 3 has no way to reach into kernel
   memory. */
#define sigInfoVirtualAddr 0x0000700000010000ull

/* How many consecutive frameSize pages, starting at sigInfoVirtualAddr, a
   fixed-size static array of physical frame addresses (signal.c) can hold
   -- sigInstallInfoPage only ever maps as many as threadCapacity()*
   sigMaxNestDepth slots actually need (2 today), so this is deliberately
   generous headroom, not a tight fit: maxThreads (sched.c) has grown by a
   fixed +8 roughly once per session for a long history of falas, and this
   comfortably absorbs many more such bumps (8 pages * frameSize/sizeof
   (SigInfo) slots-per-page / sigMaxNestDepth == room for hundreds of
   threads) before ever needing to be revisited, the same "generous
   headroom, not exact" reasoning sigMaxNestDepth's own comment (sched.h)
   already gives for itself. */
#define sigInfoMaxPages 8

typedef enum {
    sigOutcomeNone = 0,
    sigOutcomeHandler = 1,
    sigOutcomeTerminate = 2,
    /* Default disposition of a catchable job-control signal (SIGTSTP only
       -- SIGSTOP never reaches sigConsumePending at all, see the file
       comment). Same contract as sigOutcomeTerminate: this function only
       REPORTS it, the caller (isr.c) must actually call threadStop and
       reschedule, since only schedule() can produce the next thread's own
       rsp to resume with. */
    sigOutcomeStop = 3
} SigOutcome;

/* Resets ALL signal state on a Thread slot to fresh defaults: no pending or
   blocked signals, no installed handlers or their masks, handler-nesting
   depth zero. Called by threadCreate/threadCreateUser (sched.c) on every
   (re)use of a table slot,
   the same explicit-field-by-field convention threadCreate/threadCreateUser
   already use for every other Thread field -- threadReap/threadDiscard's
   own memset() of the whole struct already guarantees this before a slot
   is ever reused, so this is defensive documentation of the invariant, not
   a fix for a leak that could otherwise happen (unlike the console-reader/
   pipe-fd leaks this subsystem's sibling code had to actually fix, which
   lived in tables OUTSIDE Thread that no memset here ever touched). */
void sigResetState(Thread* thread);

/* Raises signal `signum` against thread `targetId`. sigKill and sigStop are
   both delegated straight to threadKill/threadStop (see file comment) and
   never touch sigPending at all. sigCont ALSO calls threadContinue as an
   unconditional side effect (resuming a Stopped target regardless of
   sigBlocked) but, unlike sigKill/sigStop, still falls through to set its
   own pending bit afterwards, so an installed handler still runs through
   the normal scan. Any other signum just sets the pending bit, then --
   if sigWouldDeliver agrees this signal is not blocked or ignored --
   calls threadWakeForSignal (sched.c) to force a Sleeping/Blocked target
   straight to Runnable early, real Unix's interruptible sleep/EINTR (see
   file comment); actual delivery itself still only ever happens later,
   off the interrupt-return path (isr.c), once that thread genuinely
   resumes. Returns false, changing nothing, for an out-of-range signum, an
   invalid/out-of-range targetId, or a target that is Dead/Unused (mirrors
   threadKill's own validation for the non-SIGKILL path, and threadKill's/
   threadStop's own for their delegated paths). */
bool sigRaise(uint32 targetId, int signum);

/* Exactly sigRaise above, plus attaching a real SigInfo (signalnum.h) to
   this specific raise -- `siCode`/`siPid`/`siStatus` become
   target->sigPendingInfo[signum] (sched.h) the moment the pending bit
   itself is set, later handed to a sigFlagSigInfo handler by
   sigConsumePending. sigRaise itself is now a thin wrapper around this
   (siCodeKernel, 0, 0 -- "no specific detail available/tracked", the
   honest default for the many pre-existing callers, including sigTest.c's
   whole KTEST suite, that only ever cared about signum/pending-bit
   semantics and never plumbed real sender/child detail through). Real
   production call sites needing genuine detail (sched.c's SIGALRM/SIGCHLD
   raises, syscall.c's sysSigSend) call this directly instead. Same return
   value, same validation, same sigKill/sigStop delegation, same early-wake
   side effect as sigRaise -- see that function's own doc comment for all
   of it, not repeated here. */
bool sigRaiseInfo(uint32 targetId, int signum, int siCode, int siPid, int siStatus);

/* Real kill(-pgid, signum)/kill(0, signum)'s group-targeted half (sched.h's
   Thread.pgid; userSigSend/userKill's negative/zero-pid convention,
   syscall.c's sysSigSend/sysKill) and keyboard.c's own real Ctrl-C/Ctrl-Z
   (SIGINT/SIGTSTP delivered to the FOREGROUND process group only, never a
   single target thread): scans every currently live thread once and calls
   sigRaiseInfo above against each one whose pgid matches, independently --
   SIGKILL/SIGSTOP/SIGCONT's own special-cased bypasses, per-target
   early-wake, and SigInfo attachment therefore all apply exactly as they
   already do for a plain, single-target send, simply repeated once per
   group member, rather than this function sharing or reimplementing any of
   that delivery logic itself. A pgid with no currently live member at all
   (nobody has ever claimed the foreground yet, or every process that once
   shared it has since died) is a harmless no-op, matching real kill(2)'s
   ESRCH for an empty target set -- reported back as false, exactly like a
   single-target sigRaiseInfo already reports false for an invalid/dead
   target. */
bool sigRaiseGroup(uint32 pgid, int signum, int siCode, int siPid);

/* Installs `handlerAddr` (a real user-code address, or sigHandlerDefault /
   sigHandlerIgnore) as thread's disposition for `signum`, `mask` as its
   sa_mask -- the additional signals sigConsumePending blocks for as long as
   THIS handler is active (stripped of sigKill/sigStop first, same as
   sigProcMask: neither can ever be blocked, so a mask cannot "achieve" it
   either) -- and `flags` (signalnum.h's sigFlagRestart, or 0) as its
   sa_flags. Returns the PREVIOUS disposition (like real signal(2)/
   sigaction(2)'s oldact->sa_handler) or sigSetFailed for an out-of-range
   signum or an attempt to touch sigKill or sigStop (both uncatchable, like
   real EINVAL on SIGKILL/SIGSTOP) -- `mask`/`flags` are stored regardless of
   whether `handlerAddr` is a real handler or a sentinel, but only ever
   consulted for a real one (sigHandlerDefault/sigHandlerIgnore never reach
   the custom-handler branch of sigConsumePending). */
uint64 sigSetHandler(Thread* thread, int signum, uint64 handlerAddr, uint64 mask, uint64 flags);

/* Blocks/unblocks/replaces thread's blocked-signal mask according to `how`
   (sigMaskBlock/sigMaskUnblock/sigMaskSetMask; any other value is a no-op),
   returning the mask as it was BEFORE this call (like real sigprocmask's
   oldset). sigKill's and sigStop's bits are silently stripped from `mask`
   first -- neither can ever be blocked, matching real Unix (sigCont CAN be
   blocked here, since only whether its handler runs is gated by
   sigBlocked -- the resume side effect in sigRaise never consults it). */
uint64 sigProcMask(Thread* thread, int how, uint64 mask);

/* The delivery decision, called from isr.c's deliverPendingSignals on
   every return-to-ring3 point for whichever thread is about to resume.
   Refuses to deliver anything (returns sigOutcomeNone, touching nothing)
   once thread->sigHandlerDepth already reached sigMaxNestDepth -- the small
   fixed context stack is full, so whatever is pending simply waits for a
   sigOnSigReturn to free a slot; this is the only depth-independent gate,
   unlike the old single "one handler at a time" bool it replaced. Otherwise
   scans sigPending & ~sigBlocked from signum 1 upward. An ignored signal
   (explicit sigHandlerIgnore, or sigHandlerDefault whose real-world default
   is Ignore -- see sigDefaultIsIgnore in signal.c) is dismissed and the
   scan continues, so it can never mask a real one behind it. Otherwise:
   default disposition -> sigOutcomeStop for sigTstp (sigDefaultIsStop) or
   sigOutcomeTerminate for everything else (caller must actually stop/
   terminate the thread, e.g. via threadStop/threadExit, and reschedule --
   this function never does, since it has no way to hand back a new rsp).
   Custom handler -> the interrupted context is pushed onto
   thread->sigSavedRegs[depth]/sigSavedBlocked[depth] (depth being
   sigHandlerDepth's value on entry, then incremented), sigBlocked becomes
   the union of what it already was, sigHandlerMasks[signum] (the sa_mask
   sigSetHandler stored), and signum's own bit (self-blocked, stripped of
   sigKill/sigStop like every other mask operation in this file), and `regs`
   is rewritten in place to enter the trampoline page (rip), call the
   handler (rax) with signum as its sole argument (rdi) -- sigOutcomeHandler.
   If sigHandlerFlags[signum] also has sigFlagSigInfo set, thread->
   sigPendingInfo[signum] (whatever sigRaiseInfo last stashed there for this
   signum, or sigRaise's own siCodeKernel/0/0 default) is additionally
   copied into this (thread id, depth) pair's slot in the shared
   sigInfoVirtualAddr page (signal.h), and rsi/rdx are rewritten too -- rsi
   to that slot's real user-visible address, rdx to 0 (no ucontext_t this
   kernel can expose) -- so the handler genuinely receives (signum,
   SigInfo*, NULL) exactly as sigaction()'s SA_SIGINFO promises. Without the
   flag, rsi/rdx are left completely untouched (whatever they already held),
   exactly as before this existed, since a plain one-argument handler never
   reads either. Before any of that pushing happens, thread->sigInRetryableSyscall is
   consumed (read, then unconditionally cleared): if it was true (this
   thread is genuinely mid a sysRead/sysWrite/sysWait retry, see sched.h)
   and signum's sigHandlerFlags lacks sigFlagRestart, `regs` -- the
   interrupted context about to be saved -- is rewritten first (rip past
   the interrupted `int $0x80`, rax to errInterrupted) so the eventual
   sigOnSigReturn resumes the caller having observed a genuine EINTR-style
   failure instead of transparently retrying; sigFlagRestart (or
   sigInRetryableSyscall already being false, e.g. a signal that interrupts
   plain running code or a nested handler) leaves `regs` untouched here,
   preserving today's transparent-retry behavior exactly.
   A DIFFERENT signal not covered by that union can still be delivered on a
   later call while this one is active -- pushed onto depth+1 as a genuinely
   NESTED handler, sigOnSigReturn popping it back off once it returns, the
   interrupted (still mid-handler) context resuming exactly where it was.
   Either way `*outSignum` (if non-null) is set to whichever signal was
   consumed. sigStop itself never appears here at all (see file comment --
   sigRaise bypasses this whole scan for it). */
SigOutcome sigConsumePending(Thread* thread, Registers* regs, int* outSignum);

/* Undoes exactly what the sigOutcomeHandler branch of sigConsumePending did
   for the MOST RECENTLY entered still-active handler (LIFO, like a real call
   stack of signal frames): pops thread->sigHandlerDepth, restoring `*regs`
   from thread->sigSavedRegs[depth] and thread->sigBlocked from
   thread->sigSavedBlocked[depth] at the popped index -- resuming the
   interrupted program (which may itself be a still-active OUTER handler, if
   this was a nested one) at the exact instruction, with the exact
   registers, it had before that signal arrived, as if it had never been
   interrupted at all.
   Returns false, changing nothing, if the thread was not actually inside a
   handler (a user program calling the sigreturn syscall on its own, never
   having been signaled, is a misuse this kernel simply ignores rather than
   trusting). */
bool sigOnSigReturn(Thread* thread, Registers* regs);

/* Allocates one physical frame, fills it with the trampoline's machine
   code (sigTrampolineStart..sigTrampolineEnd, sigTrampoline.S -- position
   independent by construction, see that file), and maps it read+execute+
   user (never write) at sigTrampolineVirtualAddr. Called exactly once from
   kmain.c, before any user thread can possibly receive a signal. */
void sigInstallTrampoline(void);

/* Allocates and maps sigInfoVirtualAddr's data page(s) -- see that
   constant's own comment for the layout and sizing. Called exactly once
   from kmain.c, alongside sigInstallTrampoline (before any user thread can
   possibly receive a sigFlagSigInfo delivery), and before ktestRunAll so
   sigTest.c's own deterministic coverage can read the same shared page a
   live ring-3 handler would. */
void sigInstallInfoPage(void);

#endif
