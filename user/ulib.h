#ifndef FROSTBYTE_ULIB_H
#define FROSTBYTE_ULIB_H

#include "syscall.h"
#include "fs.h"
#include "signalnum.h"
#include "poll.h"

typedef void (*SigHandler)(int signum);

/* Real sigaction()'s sa_sigaction shape: signum (same as SigHandler's own
   argument, kept for parity -- see signalnum.h's SigInfo.siSignum comment),
   a pointer to the real SigInfo describing THIS delivery, and a context
   pointer this kernel always hands back as NULL (see signalnum.h's
   sigFlagSigInfo comment for why). Only ever meaningful for a handler
   installed through userSigActionInfo below, WITH sigFlagSigInfo actually
   set in its flags -- see that function's own doc comment. */
typedef void (*SigInfoHandler)(int signum, SigInfo* info, void* context);

long userWrite(int fd, const char* buffer, unsigned long length);
long userRead(int fd, char* buffer, unsigned long length);
void userExit(int code);
long userYield(void);
long userGetpid(void);
void* userSbrk(long increment);
void userSleep(unsigned long milliseconds);

/* Real alarm(2): schedules sigAlrm against the calling process `seconds`
   real seconds from now, replacing any alarm already pending; 0 cancels
   instead of scheduling. Returns the whole seconds remaining on whatever
   alarm was pending before this call (rounded up), or 0 if none was. See
   sched.h's threadSetAlarm for the full contract. */
unsigned long userAlarm(unsigned long seconds);

/* Real poll(2): waits for any of `nfds` PollFd entries (poll.h) to become
   ready, filling each one's revents in place. timeoutMs < 0 waits forever,
   0 reports current readiness without ever blocking, > 0 blocks at most
   that many milliseconds (rounded up to this kernel's ~10ms PIT tick
   granularity -- see sched.c's threadSetAlarm for the same rounding on a
   different timer). Returns the number of entries with a nonzero revents
   (0 on timeout, never negative except -1 for a bad nfds/pointer). */
long userPoll(PollFd* fds, unsigned long nfds, long timeoutMs);

long userOpen(const char* path, unsigned int flags);
long userClose(int fd);
long userLseek(int fd, long offset, int whence);
long userStat(const char* path, FbStat* out);
long userGetdents(int fd, FbDirent* out, unsigned long length);
long userMkdir(const char* path);
long userUnlink(const char* path);

/* Real rmdir(2): the counterpart userUnlink above refuses a directory
   (EISDIR); this refuses anything that is NOT an empty directory
   (ENOTDIR/ENOTEMPTY, both collapsed to -1 here, matching userUnlink's own
   "just -1" error shape). */
long userRmdir(const char* path);

/* Real chmod(2): overwrites `path`'s whole owner-rwx mode (fs.h's
   fsModeOwner*, e.g. 0644) with `mode` -- no symbolic +x-style merge,
   matching real chmod's own numeric-mode form. 0 on success, -1 for a
   missing path or (tmpfs only) a node under a non-writable mount, same
   restriction userUnlink/userRmdir already have on the ustar-loaded
   backing store. Takes effect immediately for any FUTURE userOpen/
   userSpawn/userExec on this path -- an fd already open, or a process
   already running, is never affected (real Unix only checks permission at
   open/exec time). */
long userChmod(const char* path, unsigned int mode);

/* Real chown(2) (vfs.h's vfsChown): reassigns `path`'s owner uid outright.
   Root-only (this process's own userGetuid() must be 0), full stop, unlike
   userChmod -- real Unix's _POSIX_CHOWN_RESTRICTED default means not even a
   file's own owner may give it away. -1 for a missing path, a node under a
   non-writable mount, or a non-root caller; the target's uid is left
   completely unchanged on any failure. */
long userChown(const char* path, unsigned int uid);

/* Real getuid(2)/setuid(2) (sched.h's Thread.uid, currentThreadUid/
   currentThreadSetUid): userGetuid always succeeds, returning THIS
   process's own current uid. userSetuid enforces the real privilege rule
   itself (no additional check happens at the syscall layer) -- 0 ("root")
   may become any uid; any other uid may only pass its own current value
   back in (a no-op that still returns 0, matching real setuid(getuid())),
   and any other target returns -1 with the credential unchanged. There is
   no way back to uid 0 once dropped short of already being uid 0 at the
   time of the call -- a real, permanent privilege drop for the lifetime of
   this process, exactly like a daemon that setuid()s away from root once
   at startup. */
long userGetuid(void);
long userSetuid(unsigned int uid);

/* Real getgid(2)/setgid(2) (sched.h's Thread.gid, currentThreadGid/
   currentThreadSetGid): userGetgid always succeeds, returning THIS
   process's own current gid. userSetgid's privilege gate is THIS process's
   own userGetuid() (== 0), not its gid -- matching real setgid(2)'s
   CAP_SETGID-shaped rule -- any non-root caller may only pass its own
   current gid back in (a no-op that still returns 0), any other target
   returns -1 unchanged. */
long userGetgid(void);
long userSetgid(unsigned int gid);

/* Real chgrp(2) (vfs.h's vfsChgrp): reassigns `path`'s group gid. Unlike
   userChown (root-only, full stop), the file's own owner may also call
   this -- but only to set the group to THIS process's own current gid
   (userGetgid()), never an arbitrary one, since this kernel tracks exactly
   one gid per process (no supplementary-groups list). Root may set any
   gid. -1 for a missing path, a node under a non-writable mount, a
   non-owner non-root caller, or a non-root caller naming any gid other
   than its own. */
long userChgrp(const char* path, unsigned int gid);

/* Real chdir(2)/getcwd(2) (sched.h's Thread.cwd, vfs.h's vfsChdir/
   vfsGetcwd): userChdir resolves a relative `path` against THIS process's
   own current cwd, requires the result to already be a real directory, and
   makes it this process's new cwd -- 0 on success, -1 (nothing changed) if
   it does not name a directory. A child spawned by userSpawn/userExec
   AFTER this call inherits the new value; this process's parent, and any
   already-spawned sibling, are never affected (each process's cwd is an
   independent copy, not shared state). userGetcwd copies the current
   value into `buffer` (nul-terminated) and returns its length, or -1
   (buffer untouched) if it does not fit `cap` -- real getcwd(2)'s own
   ERANGE case. */
long userChdir(const char* path);
long userGetcwd(char* buffer, unsigned long cap);

/* Real getenv(3)/setenv(3) (sched.h's Thread.env, syscall.c's sysGetEnv/
   sysSetEnv): userGetEnv copies THIS process's own value for `name` into
   `buffer` (nul-terminated) and returns its length, or -1 (buffer
   untouched) if `name` has no entry or the value does not fit `cap`.
   userSetEnv overwrites an existing entry with a matching `name`, or
   creates a new one, in THIS process's own table only -- 0 on success, -1
   if the combined "NAME=VALUE" would not fit the kernel's fixed entry size
   or the table is already full. A child spawned by userSpawn/userExec
   AFTER a userSetEnv call inherits the new value (a real, independent
   copy, same as userChdir above); this process's parent, and any
   already-spawned sibling, are never affected. */
long userGetEnv(const char* name, char* buffer, unsigned long cap);
long userSetEnv(const char* name, const char* value);

/* Real unsetenv(3): removes THIS process's own entry named `name` from its
   table, if any, so a following userGetEnv on the same name returns -1
   again -- 0 whether or not an entry actually existed (matching real
   unsetenv(3), a no-op success either way), never touches the parent's or
   any sibling's own table (same independent-copy shape as userSetEnv). */
long userUnsetEnv(const char* name);

long userPipe(int fds[2]);

/* Shared pipes: unlike userPipe (both ends always land in the caller's own
   fd table), userPipeCreate returns a raw handle number that means nothing
   until an fd table actually attaches to it (userPipeAttach) -- meant to be
   handed to a different, independently spawned process (e.g. as an argv
   string) so the two can share a pipe without needing fork(). Once
   attached, userRead/userWrite on that fd really block (park the whole
   process, not busy-retry) instead of returning -1 for "try again" the way
   an unshared userPipe end does. */
long userPipeCreate(void);
long userPipeAttach(long handle, int wantWrite);
long userSymlink(const char* target, const char* path);
long userReadlink(const char* path, char* buffer, unsigned long cap);
long userLink(const char* existingPath, const char* newPath);

/* Named pipe: creates a fifo at `path` (see vfsMkfifo, vfs.h). Unlike
   userPipeCreate/userPipeAttach, which need a raw handle number passed to
   both ends (e.g. as an argv string, see fifomaster.c), any two processes
   that simply agree on a path can rendezvous by both calling userOpen on it
   -- oWriteOnly for a blocking write end, oReadOnly for a blocking read
   end. 0 on success, -1 if the path already exists or its parent is not a
   writable directory. */
long userMkfifo(const char* path);
/* argv is a NULL-terminated array of C strings, exactly like execv(3)'s --
   pass nullptr for "no arguments" (argc 0 on the other side). */
long userExec(const char* path, char* const argv[]);
long userSpawn(const char* path, char* const argv[]);
long userWait(long pid);

/* waitpid(-1)-style "wait for ANY child, not one specific pid": reaps
   whichever child of the caller happens to already be dead (or blocks, the
   same real blocking semantics as userWait(pid), until one dies) and
   returns its exit code. Writes the reaped child's own pid to *outPid
   (ignored if NULL). Returns -1 immediately, without blocking, if the
   caller currently has no children at all, dead or alive -- the real
   waitpid(-1)'s ECHILD case. See user/waitanytest.c for a live demo. */
long userWaitAny(long* outPid);

/* Unconditionally terminates another process by pid -- the only way to
   recover a process genuinely stuck forever (e.g. blocked in userRead on a
   shared pipe whose writer never shows up, see userPipeAttach). 0 on
   success, -1 for an invalid pid, an already-dead one, or your own. The
   killed process gets no chance to run any cleanup -- its parent's
   userWait still reports its exit status, which will be exactly 9. Unlike
   userSigSend below, this can never be caught, blocked, or ignored --
   internally it goes straight to threadKill (sched.c), the same as
   userSigSend(pid, sigKill) does (signal.h).

   Real kill(2)'s own pid<=0 convention (sched.h's Thread.pgid) also
   applies here: pid==0 kills every OTHER member of the caller's own
   process group (never the caller itself -- a Running thread is never one
   of threadKill's accepted states, the same reason plain self-targeting
   already does nothing special); pid<0 kills every member of group -pid.
   0 is still returned for success as long as AT LEAST ONE target thread
   was actually killed (real kill(2)'s own ESRCH-if-nobody-matched
   semantics for the group form). */
long userKill(long pid);

/* Real, catchable signal delivery (signal.h) -- see that header for the
   full design (a shared trampoline page, delivery on return-to-ring3,
   sysSigReturn to resume exactly where a handler interrupted). */

/* Raises signal `signum` (signal.h's sigHup..sigTstp) against `pid`. 0 on
   success, -1 for a bad signum or an invalid/dead/unused target. sigKill
   is delegated straight to the same unconditional path userKill uses --
   it is never caught, blocked, or ignored, matching real kill(2). Any
   other signal is only actually delivered once the target next returns to
   user code on its own (this kernel does not wake a sleeping/blocked
   target early -- see signal.h's file comment).

   Same pid<=0 group convention as userKill above: pid==0 targets the
   caller's own process group (INCLUDING itself, unlike userKill's group
   form -- sigRaiseInfo has no "never self" exclusion the way threadKill
   does), pid<0 targets group -pid. See sched.h's Thread.pgid and
   signal.h's sigRaiseGroup for the full design, and user/pgroupfg.c/
   user/pgroupbg.c for a live demo of the SAME scoping rule applied by
   keyboard.c's own real Ctrl-C. */
long userSigSend(long pid, int signum);

/* Real pidfd_open(2): opens a handle on process `pid` that stays correct
   even if `pid`'s own numeric slot is later reaped and handed to a
   completely different, unrelated process (pidfd.h/sched.h's
   Thread.generation) -- unlike userKill/userSigSend/userWait above, which
   all still take a raw pid the exact same way real kill(2)/waitpid(2) do
   (and so share their exact same well-known pid-reuse race, deliberately
   left unfixed for them, see sched.c's allocSlot doc comment). Returns a
   small non-negative fd on success (closed with the ordinary userClose),
   or -1 for pid 0, an out-of-range pid, an already-dead-and-reaped or
   never-existed pid, or a full pidfd table. Opening a pidfd does NOT keep
   the target alive in any way -- it can still exit and be reaped by
   anyone else in the meantime, exactly like a raw pid could; the whole
   point of this handle is only ever noticing that when it happens, instead
   of silently acting on whoever now sits at that same number. */
long userPidfdOpen(long pid);

/* Sends signal `signum` to whichever process `fd` (from userPidfdOpen)
   still safely refers to. 0 on success, -1 for a bad signum, an `fd` that
   is not an open pidfd at all, or one whose original target has since
   been reaped or recycled -- the failure userSigSend(cachedPid, signum)
   can never distinguish from "the signal was delivered to a live but
   completely unrelated process that happens to share that old pid now".
   Otherwise identical semantics to userSigSend's own single-target path
   (sigKill still unconditional and uncatchable, signum still delivered on
   the target's own next return to user code). */
long userPidfdSend(long fd, int signum);

/* Real (partial) waitid(2)-over-a-pidfd: blocks until the process `fd`
   still safely refers to actually exits, then reaps it and returns its
   exit code -- the same real blocking semantics as userWait(pid), except
   -1 also covers "the original target was reaped by someone else while
   this call was waiting", which userWait(pid) has no way to report (it
   would simply keep waiting on whichever unrelated thread now occupies
   that pid instead). See user/pidfdtest.c for a live ring-3 demonstration
   of a pidfd correctly reporting a just-reaped target as gone through the
   real syscall path (not just the in-kernel unit tests), and
   schedTest.c's pidfd tests for a deterministic, create/discard/recreate
   reproduction of the full scenario -- a stale handle whose slot has
   since been handed to a brand new, unrelated, live thread -- that a live
   boot cannot reliably force on demand (allocSlot's own round-robin
   headroom makes hitting one specific freed slot again take dozens-to-
   hundreds of unrelated allocations first, see its doc comment, sched.c). */
long userPidfdWait(long fd);

/* Real mmap(2): addrHint of 0 asks the kernel to pick the address itself
   (mmap.c's mmapReserve, first-fit inside this process's own mmap region);
   a nonzero addrHint asks for that EXACT address, refused (not silently
   relocated) if it does not fit -- there is no MAP_FIXED bit here, every
   addrHint already behaves like real mmap's own weaker "hint" narrowed
   down to "exact or refused". prot is fs.h's mmapProtRead/Write/Exec
   (mmapProtExec IS hardware-enforced -- executing a mapping that never
   asked for it takes a real SIGSEGV -- see fs.h's own doc comment for the
   exact scope, including the CPU-support gate and the same enforcement
   applied to a binary's own ELF image and its stack/brk); flags is fs.h's
   mmapAnonymous or 0. When mmapAnonymous is NOT
   set, `fd` must already be open on a real seekable file (fdFile/fdSfs/
   fdBlockDev -- the same three kinds userLseek's own vfsLseek already
   restricts to) and `offset` must be frameSize-aligned; both are ignored
   for an anonymous mapping. No physical memory is actually touched until
   the first real access to a given page -- see mmap.h's own file comment
   for the full demand-paging contract, including why closing `fd` before
   every page has been faulted in leaves the rest of this mapping unable
   to fill (an honestly scoped limitation, not a hidden bug). Returns the
   mapped base address, or (void*)-1 (compare against userMmapFailed) on
   any failure: bad alignment, a range outside this process's own mmap
   region, overlap with an existing mapping, a bad/wrong-kind fd for a
   file-backed mapping, or no mapping slot/free region left. */
void* userMmap(void* addrHint, unsigned long length, int prot, int flags, int fd, long offset);

/* Sentinel userMmap returns on failure, real MAP_FAILED's own value --
   never a real mapped address, since every real one falls inside this
   process's own bounded 256 MiB elfSlotSize slot (elf.h), nowhere near
   the top of the address space. */
#define userMmapFailed ((void*)-1)

/* Real munmap(2): see mmap.h's own mmapRelease for the full contract
   (partial unmap shrinks the mapping in place, a range entirely inside
   one mapping's interior splits it, already-unmapped memory is a no-op).
   0 on success, -1 for a misaligned addr, a zero length, or (a real,
   if unlikely, resource failure) no free mapping slot left to hold a
   split's own second half. */
long userMunmap(void* addr, unsigned long length);

/* Installs `handler` as the calling process's own disposition for
   `signum` (nullptr/sigHandlerDefault restores the default action,
   sigHandlerIgnore silently discards it), `mask` as its sa_mask -- the
   additional signals blocked for as long as THIS handler is running (on top
   of whatever was already blocked and signum's own bit, always self-blocked
   like real Unix's implicit default) -- and `flags` as its sa_flags
   (signalnum.h's sigFlagRestart, or 0). A signal NOT covered by `mask` can
   still interrupt this handler with a genuinely nested one of its own
   (see signal.h's file comment) rather than waiting for it to return.
   Without sigFlagRestart, a signal that interrupts THIS process blocked in
   userRead/userWrite/userWait makes that call return errInterrupted once
   the handler returns instead of transparently resuming it (real EINTR);
   sigFlagRestart opts back into the transparent-resume behavior every
   handler had before this flag existed. Returns the PREVIOUS disposition,
   or -1 for a bad signum or an attempt to touch sigKill (uncatchable, like
   real EINVAL on SIGKILL). `handler` is called with the signal number as
   its only argument; it must return normally (not userExit, not
   longjmp-style tricks this kernel does not support) for execution to
   resume exactly where the signal interrupted. */
long userSigAction(int signum, SigHandler handler, unsigned long mask, unsigned long flags);

/* Identical wire format to userSigAction (same syscall, same signum/mask
   validation, same PREVIOUS-disposition return value) -- the only
   difference is `handler`'s real C type, SigInfoHandler instead of
   SigHandler, so a caller does not need its own cast at the call site. The
   kernel itself does not distinguish the two function-pointer shapes at
   all (it only ever stores/calls a raw address, see signal.c's
   sigConsumePending): what actually determines whether the handler is
   CALLED with one argument or three is whether `flags` includes
   sigFlagSigInfo (signalnum.h) -- the caller must include it here
   deliberately, exactly like real sigaction()'s SA_SIGINFO. Passing a
   SigInfoHandler through this function WITHOUT that flag set (or, equally,
   passing a plain SigHandler through this function WITH it set) is a
   caller bug this kernel does not protect against, mirroring real C's own
   union-based sa_handler/sa_sigaction footgun -- the handler would run
   with whatever raw rsi/rdx happened to hold, not a valid SigInfo
   pointer. */
long userSigActionInfo(int signum, SigInfoHandler handler, unsigned long mask, unsigned long flags);

/* Blocks (sigMaskBlock), unblocks (sigMaskUnblock), or replaces
   (sigMaskSetMask) the calling process's blocked-signal mask, returning
   the mask as it was immediately before this call. A blocked signal still
   gets raised (sigPending) but is not delivered until unblocked. */
unsigned long userSigProcMask(int how, unsigned long mask);

/* Real setpgid(2): moves process `pid` (0 means the caller itself) into
   process group `pgid` (0 means "become a new group leader, using pid's
   own resolved id as the group id" -- the same "0 means self/derive from
   self" shape sysSetFgPgrp does NOT have, since a foreground group is
   named directly, never derived). 0 on success, -1 for a pid that does not
   name any currently live thread (sched.h's threadSetPgid; id 0, the idle
   thread, is also always rejected -- see that function's own doc comment).
   No session/permission check at all -- see threadSetPgid's own honestly-
   documented omission. */
long userSetpgid(long pid, long pgid);

/* Real getpgid(2): returns process `pid`'s current process group (0 means
   the caller itself), or -1 for a pid that does not name any currently
   live thread. */
long userGetpgid(long pid);

/* Real setpriority(PRIO_PROCESS, pid, ...)/nice(2)'s underlying mechanism:
   sets process `pid`'s (0 means the caller itself) CFS-lite scheduling
   weight (sched.h's threadSetPriority) to `weight` directly -- higher runs
   proportionally more often, exactly like kmain.c's own kernel-internal
   workerA/workerB demo already proves for a kernel-created thread, now
   reachable for any real userspace process including one the caller did
   not create itself. Unlike real nice(), takes an absolute weight rather
   than a relative increment, and has no privilege check at all (any
   process may renice any other) -- this kernel's already-documented,
   honest lack of a uid/permission model (see threadSetPgid's own comment
   for the same omission). 0 on success, -1 for a `pid` that does not name
   any currently live thread, the idle thread (pid 0's OWN id, not the
   `pid == 0` self-convention above), or a `weight` of 0. */
long userNice(long pid, long weight);

/* Real getpriority(PRIO_PROCESS, pid)-equivalent: returns process `pid`'s
   (0 means the caller itself) current CFS-lite scheduling weight, or -1
   for a pid that does not name any currently live thread -- unambiguous
   because a real weight is always > 0 (userNice above always rejects
   weight 0, and every thread starts at schedDefaultWeight, sched.h). */
long userGetPriority(long pid);

/* Real time(NULL): seconds since the Unix epoch, off the kernel's real
   CMOS RTC (rtc.h/syscall.c's sysTime) -- every other notion of time this
   ulib exposes (userSleep, userAlarm) is relative to boot, not the
   calendar. Never fails; a hardware read failure comes back as 0
   (rtc.c's rtcNow contract), the same "epoch itself" sentinel real
   time(2) would return for 1970-01-01T00:00:00Z. */
long userTime(void);

/* Real tcsetpgrp(3): names `pgid` as the process group keyboard.c's real
   Ctrl-C/Ctrl-Z (SIGINT/SIGTSTP) is delivered to from now on -- see
   keyboard.h's keyboardSetForegroundPgid for the full contract, including
   why 0 (before anything ever calls this) safely means "nobody". Always
   succeeds (returns 0): unlike a real terminal driver, this kernel has no
   session/controlling-terminal ownership to check the caller against. */
long userSetForegroundPgid(long pgid);

/* Real tcgetpgrp(3): the pgid userSetForegroundPgid (by anyone) last set,
   or 0 if nobody ever has since boot. */
long userGetForegroundPgid(void);

void print(const char* text);
void printDec(long value);
unsigned long stringLength(const char* text);

/* Writes value's decimal representation (with a leading '-' if negative)
   into out, nul-terminated -- unlike printDec, which prints it directly,
   this is for building an argv string to hand a value (e.g. a shared pipe
   handle, see userPipeCreate/userSpawn) to a child process. out must have
   room for at least 21 bytes (20 digits for INT64_MIN plus the sign, plus
   the terminator). */
void decToString(long value, char* out);

/* Parses the decimal integer (optionally '-'-prefixed) at the start of
   text, stopping at the first non-digit -- the inverse of decToString, for
   a child process to recover a value (e.g. a shared pipe handle) passed to
   it as an argv string. No error signaling: text that starts with no
   digits at all (after an optional sign) parses as 0, matching how a
   deliberately small, trusted set of internal callers uses it. */
long parseDec(const char* text);

#endif
