#ifndef FROSTBYTE_SIGNALNUM_H
#define FROSTBYTE_SIGNALNUM_H

/*
 * Plain numeric constants shared verbatim between the kernel (signal.h/
 * signal.c) and userspace (user/ulib.h/ulib.c) -- exactly like syscall.h
 * already is for syscall numbers. Split out from signal.h specifically so
 * user code never has to include signal.h itself, which pulls in sched.h's
 * Thread struct and isr.h's Registers -- kernel-only types a freestanding
 * user program has no business seeing, let alone compiling against.
 */

/* Signal numbers. A small, real (not exhaustive) POSIX-shaped subset --
   enough to exercise catchable delivery, ignore, default-terminate and the
   one uncatchable case meaningfully, without pretending to implement every
   job-control signal a real kernel would. Numbering matches Linux/x86-64
   for anything actually used here, so an unused gap (5, 7, 16) does too. */
#define sigHup   1
#define sigInt   2
#define sigQuit  3
#define sigIll   4
#define sigAbrt  6
#define sigFpe   8
#define sigKill  9
#define sigUsr1  10
#define sigSegv  11
#define sigUsr2  12
#define sigPipe  13
#define sigAlrm  14
#define sigTerm  15
#define sigChld  17
#define sigCont  18
#define sigStop  19
#define sigTstp  20

/* Handler-slot sentinels (Thread.sigHandlers[signum] kernel-side,
   userSigAction's `handler` argument userspace-side), distinct from any
   real user code address since every user ELF slot starts at
   userSpaceMin == 0x40000000 (syscall.c), far above 1. */
#define sigHandlerDefault 0ull
#define sigHandlerIgnore  1ull

/* sigSetHandler's/userSigAction's failure return -- mirrors real
   signal(2)'s SIG_ERR, distinguishable from any real sentinel/handler
   address a caller could have gotten back as the *previous* disposition.
   Written without a uint64 cast (unlike the kernel-only constants in
   signal.h) so this header stays includable from userspace, which never
   sees types.h -- see fs.h's FbStat for the same convention. */
#define sigSetFailed (~0ull)

/* sigProcMask/userSigProcMask "how" values (mirrors POSIX SIG_BLOCK/
   SIG_UNBLOCK/SIG_SETMASK by shape, not by number). */
#define sigMaskBlock    0
#define sigMaskUnblock  1
#define sigMaskSetMask  2

/* sigSetHandler's/userSigAction's `flags` argument (mirrors POSIX
   sigaction's sa_flags by shape, not by bit position -- there is only one
   flag here). Default (0, what every handler got before this bit existed)
   is real POSIX's own default: a signal delivered while a thread sits
   blocked inside a slow syscall (read/write/wait -- see signal.c's
   sigInRetryableSyscall) does NOT transparently resume that syscall once
   the handler returns, it makes the syscall return errInterrupted instead,
   exactly like an EINTR on real Unix. Passing sigFlagRestart opts back into
   the OTHER behavior (real SIG_RESTART/BSD's original signal(3) semantics):
   the interrupted syscall is retried, invisibly to the caller, once the
   handler returns. Only meaningful for a signal that actually interrupts a
   genuinely retryable syscall; for anything else (plain running code, a
   sleep, a signal that does not even install a handler) it is silently
   irrelevant, same as real SA_RESTART. */
#define sigFlagRestart (1ull << 0)

/* Mirrors POSIX sigaction's SA_NODEFER: by default (this bit clear), a
   signal's own number is added to sigBlocked for as long as ITS handler is
   running (signal.h's sigConsumePending, "self-blocked like real Unix's
   implicit default") -- so the same signal raised again while its handler
   is still active stays merely pending, delivered only once that handler
   returns, never nested into itself. Passing sigFlagNoDefer for a signal
   disables exactly that one self-block (nothing else -- sa_mask/other
   signals' self-blocks are untouched): a second raise of that same signal
   while its own handler is active is then genuinely deliverable, entering
   a NESTED instance of its own handler, the same mechanism a completely
   different unmasked signal already uses to nest (see signal.h's
   sa_mask paragraph) -- SA_NODEFER only widens which signal numbers that
   mechanism applies to, it does not introduce a new one. Irrelevant for a
   signal that never raises itself from inside its own handler. */
#define sigFlagNoDefer (1ull << 1)

/* Mirrors POSIX sigaction's SA_SIGINFO: by default (this bit clear) a
   handler is called with exactly one argument, the signal number (this
   kernel's ONLY shape until now, `regs->rdi = signum` in signal.c's
   sigConsumePending). Passing sigFlagSigInfo for a signal tells
   sigConsumePending to ALSO populate a real SigInfo (below) describing this
   specific delivery -- who/what raised it, real Unix's siginfo_t.si_pid/
   si_code/si_status -- and hand the handler a pointer to it as a SECOND
   argument (rsi), plus a third (rdx, always 0/NULL here: this kernel has no
   ucontext_t of its own to expose, unlike a real kernel's SA_SIGINFO third
   argument). A handler registered with this flag must actually be declared
   to take (int, SigInfo*, void*) -- see ulib.h's SigInfoHandler/
   userSigActionInfo -- exactly like real sigaction()'s sa_sigaction union
   member only being valid to read when SA_SIGINFO was actually passed. */
#define sigFlagSigInfo (1ull << 2)

/* si_code values SigInfo.siCode below can hold -- a small, real (not
   exhaustive) subset of POSIX's SI_USER/SI_KERNEL/CLD_EXITED/CLD_KILLED/
   CLD_STOPPED/CLD_CONTINUED, numbered independently of them (this kernel's
   own small namespace, not a Linux ABI match, unlike the signal numbers
   above which deliberately do match). siCodeUser: signum was raised by
   another process's real userSigSend/kill(2)-shaped syscall (siPid is the
   SENDER's own pid). siCodeKernel: raised by the kernel itself with no
   specific sending process (SIGALRM's real timer expiry; also sigRaise's
   own thin-wrapper default for any caller that supplies no detail at all).
   siCodeChld{Exited,Killed,Stopped,Continued}: sigChld specifically,
   distinguishing WHY a child's status changed (siPid is the CHILD's pid,
   siStatus its real exit code for Exited, threadKilledExitCode for Killed,
   or the job-control signal number for Stopped/Continued) -- real Unix's
   own WIFEXITED/WIFSIGNALED/WIFSTOPPED/WIFCONTINUED distinction, exposed
   directly instead of needing a separate macro to decode a packed status
   word the way real wait(2)'s plain `int status` still does. */
#define siCodeUser          0
#define siCodeKernel        1
#define siCodeChldExited    2
#define siCodeChldKilled    3
#define siCodeChldStopped   4
#define siCodeChldContinued 5

/* Real siginfo_t, cut down to exactly the fields this kernel ever actually
   populates (see siCode* above for what each one means per code) -- shared
   verbatim between kernel and userspace like every other type in this
   header, which is exactly why it is built from plain `int`, not a
   types.h-only typedef (see this header's own file comment). siSignum
   duplicates the handler's own first argument (signum) -- real siginfo_t
   keeps si_signo too, for the same reason: a single handler function
   registered against more than one signal number can tell them apart from
   *info alone, without needing its own first argument at all. Handed to a
   sigFlagSigInfo handler as a real pointer (signal.c's sigConsumePending)
   into a small kernel-owned, user-readable (never user-writable) page
   mapped at a fixed address -- see signal.h's sigInfoVirtualAddr -- the
   same "one shared page, like a vDSO" trick this kernel's trampoline page
   already uses, not memory the handler owns or must free. */
typedef struct SigInfo {
    int siSignum;
    int siCode;
    int siPid;
    int siStatus;
} SigInfo;

/* A retryable syscall (sysRead/sysWrite/sysWait -- see syscall.c) that got
   interrupted by a signal without sigFlagRestart returns this instead of
   retrying. Mirrors a raw Linux syscall's own convention of returning
   -errno directly (here, -EINTR, whose real errno value is 4) rather than
   a separate errno variable, since this kernel has no per-thread errno of
   its own to set. Written without a uint64 cast, like every other
   userspace-visible constant in this header, so it stays includable from
   freestanding user code. */
#define errInterrupted (-4)

#endif
