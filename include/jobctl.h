#ifndef FROSTBYTE_JOBCTL_H
#define FROSTBYTE_JOBCTL_H

#include "types.h"

/*
 * Generic "run a binary by path, with real job control" helpers, callable
 * only from plain kernel-thread context (shell.c's shellRun, or a boot-time
 * demo thread in kmain.c) -- never from inside a syscall's interrupt-gate
 * handler, exactly like keyboardReaderReadBlocking/pipeReadBlocking's own
 * restriction, since jobctlWaitForeground below parks with threadJoin's
 * real halt() loop.
 *
 * Unlike syscall.c's doSpawn (sysSpawn's underlying implementation, used by
 * one ring-3 process to spawn another), a job started here always becomes
 * its OWN new process group leader (threadSetPgid(pid, pid)) regardless of
 * the caller's own pgid -- matching a real shell's fork()+setpgid(child,
 * child) convention for every new job/pipeline, not doSpawn's default of
 * inheriting the parent's group (right for a process spawning a plain
 * worker of its own, wrong for an interactive shell launching a new
 * foreground/background command). argv is already kernel memory here (the
 * caller IS the kernel, not a ring-3 process going through a syscall), so
 * neither function copies or range-checks it the way copyUserArgv/
 * userRangeOk (syscall.c) must for a user-supplied pointer.
 */

/* Loads `path` and spawns it as a new process group leader, hands it "the
   terminal" (keyboardSetForegroundPgid, keyboard.h) immediately, and
   returns its pid without waiting -- split from the actual wait
   (jobctlWaitForeground below) so a caller can learn the pid the instant
   the job exists, not only after it has already exited (shell.c's own
   background job table needs the same immediacy from
   jobctlRunBackground below, so this mirrors it instead of hiding the pid
   inside a single combined call). Returns threadInvalidId, touching the
   terminal not at all, if `path` cannot be loaded (missing from the VFS,
   malformed ELF, or the thread table is full). */
uint32 jobctlSpawnForeground(const char* path, const char* const* argv, int argc);

/* Blocks (threadJoin) until the pid jobctlSpawnForeground just returned
   exits, then reclaims the terminal for the CALLING thread's own group --
   deliberately not "whatever pgid owned the terminal right before
   jobctlSpawnForeground ran" (which could itself be some earlier, now-dead
   job's stale id): a real shell always gets its own prompt's terminal back
   when a foreground job quits, so a later Ctrl-C typed at the prompt
   reaches the shell's NEXT job, not a group nothing will ever answer for
   again. Returns the job's real exit code, same contract as threadJoin
   itself. */
int64 jobctlWaitForeground(uint32 pid);

/* Loads `path` and spawns it as a new process group leader exactly like
   jobctlSpawnForeground, but never touches the foreground pgid at all and
   returns the new pid immediately without waiting -- a real shell's `&`
   suffix. The caller is responsible for eventually reaping it (e.g.
   shell.c's own background job table, reaped opportunistically via
   threadJoinAnyChild). Returns false (touching *outPid not at all) on the
   same load/spawn failures as jobctlSpawnForeground. */
bool jobctlRunBackground(const char* path, const char* const* argv, int argc,
                          uint32* outPid);

/* Generalizes jobctlSpawnForeground/jobctlRunBackground's shared "load and
   spawn" step for shell.c's new `cmd1 | cmd2` pipeline syntax: `stdinHandle`/
   `stdoutHandle` are raw pipe.h handles from vfsPipeCreateHandle (vfs.h),
   baked into the new process's own fd 0/fd 1 (vfsPipeAttachInto) BEFORE it
   ever runs a single instruction -- -1 for either one leaves that fd at its
   normal default (the real console), exactly like an unpiped command's own
   stdin/stdout. `joinPgid` is threadInvalidId for "become a new group
   leader" (jobctlSpawn's existing default, what every other caller here
   still wants) or an already-live pgid to join instead -- a pipeline's
   second-and-later stage joins its first stage's own new group so a single
   Ctrl-C reaches every stage at once, exactly like a real shell's pipeline
   job. Already fully general for a MIDDLE stage of a longer pipeline too:
   `stdinHandle`/`stdoutHandle` are independent, so a stage can have both a
   piped stdin AND a piped stdout at once (shell.c's runPipelineStages does
   exactly this for every interior stage of a 3+-stage pipeline) -- the only
   things that ever needed generalizing for that were shell.c's own
   parsing/orchestration and jobctlWaitPipelineStages below, never this
   function. Never touches the terminal itself: shell.c's own pipeline
   runner grants and reclaims it once, for the pipeline as a whole, via
   jobctlWaitPipelineStages below -- not per stage. Returns threadInvalidId
   on the same load/spawn failures as jobctlSpawnForeground. */
uint32 jobctlSpawnPiped(const char* path, const char* const* argv, int argc,
                         int stdinHandle, int stdoutHandle, uint32 joinPgid);

/* Blocks until every one of `pids[0..count-1]` (jobctlSpawnPiped's own
   return values for a pipeline's stages, `pids[1..count-1]` already joined
   to `pids[0]`'s group) has exited, then reclaims the terminal for the
   CALLING thread's own group -- the same two-step contract
   jobctlWaitForeground has for a single job, just for a whole N-stage
   pipeline at once (2 <= count <= shellMaxPipelineStages, shell.h). Grants
   the terminal to `pids[0]`'s group (which every other pid is already a
   member of) before waiting, exactly like jobctlSpawnForeground already
   does for a single job, so a Ctrl-C typed while the pipeline runs reaches
   every stage. threadJoin's own real blocking semantics mean joining every
   pid in this fixed order is correct regardless of which stage actually
   exits first. Returns the LAST stage's exit code, matching a real
   pipeline's own `$?` convention (the last command's status). */
int64 jobctlWaitPipelineStages(const uint32* pids, int count);

/* Installs `inPath`/`outPath`/`errPath` file redirections (vfsOpenInto,
   vfs.h) into an ALREADY-SPAWNED child's fd 0/fd 1/fd 2 -- shell.c's new
   `cmd [< IN] [> OUT | >> OUT] [2> ERR | 2>> ERR]` syntax. Any path may be
   nullptr to leave that fd untouched (still whatever jobctlSpawn/
   jobctlSpawnPiped already gave it -- the real console by default, or a
   pipe end for a pipeline stage). `outAppend`/`errAppend` select `>>`/
   `2>>` (oAppend, never truncates) over `>`/`2>` (oTrunc) when the
   matching path is non-null; ignored otherwise. fd 0's default console
   reader is released first, exactly like jobctlSpawnPiped's own stdin
   override above, so redirecting it never leaks that reader slot; fd 1
   and fd 2 have no such resource (their own default, fdConsole, owns
   nothing -- neither is ever handed a reader by vfsFdInit). Exposed as
   its own function (rather than folded into jobctlSpawnPiped's already-
   long parameter list) so shell.c's `runPipeline` can layer a file
   redirect onto exactly one end of a pipeline stage -- the other end
   already wired to the shared pipe by jobctlSpawnPiped -- without a
   combinatorial explosion of pipe-handle vs. path parameters on one call.
   Returns false, leaving the child alive and running with whatever fds it
   already had, on a bad path (missing without oCreate's implicit
   create-on-write, unwritable parent, a fifo opened with the wrong mode,
   exhausted pipe/inode resources) -- the caller decides whether that
   warrants killing it (every caller in this kernel currently does, via
   threadKill+threadJoin, exactly like runPipeline already does for a
   failed second-stage spawn). */
bool jobctlApplyRedirects(uint32 pid, const char* inPath, const char* outPath, bool outAppend,
                           const char* errPath, bool errAppend);

/* Same as jobctlSpawnForeground, but installs `inPath`/`outPath`/`errPath`
   file redirections (jobctlApplyRedirects above) into the child's fd 0/
   fd 1/fd 2 BEFORE the terminal is ever granted to it -- shell.c's
   `NAME [ARGS] [< IN] [> OUT | >> OUT] [2> ERR | 2>> ERR]`. On a redirect
   failure the half-spawned child is killed and reaped before returning
   threadInvalidId, exactly as if the load/spawn itself had failed: the
   terminal must never be granted to a child whose own stdio could not be
   wired up as asked. */
uint32 jobctlSpawnForegroundRedirected(const char* path, const char* const* argv, int argc,
                                        const char* inPath, const char* outPath, bool outAppend,
                                        const char* errPath, bool errAppend);

/* Same as jobctlRunBackground, but with the same file-redirection support
   as jobctlSpawnForegroundRedirected above -- shell.c's
   `NAME [ARGS] [< IN] [> OUT | >> OUT] [2> ERR | 2>> ERR] &`. Returns
   false (touching *outPid not at all) on the same load/spawn failures as
   jobctlRunBackground, or on a redirect failure (the half-spawned child is
   killed and reaped first, same as the foreground case). */
bool jobctlRunBackgroundRedirected(const char* path, const char* const* argv, int argc,
                                    const char* inPath, const char* outPath, bool outAppend,
                                    const char* errPath, bool errAppend,
                                    uint32* outPid);

/* Generalizes jobctlSpawnPiped for shell.c's new `cmd1 <<< TEXT | cmd2`
   pipeline syntax, the same way jobctlSpawnPiped itself generalizes
   jobctlSpawnForeground/jobctlRunBackground: spawns `path` with fd 0 wired
   to a brand-new here-string pipe (`text` plus a trailing newline, written
   and closed by THIS kernel thread before returning, exactly like
   jobctlSpawnForegroundHereString below) instead of the console, AND
   `stdoutHandle`/`joinPgid` passed straight through to jobctlSpawnPiped so
   the spawned process can simultaneously be a pipeline's first stage --
   -1/threadInvalidId for either one leaves that side at jobctlSpawnPiped's
   own plain defaults (console stdout, new group leader). shell.c's
   runPipeline uses this for a pipeline's LEFT stage when it was written
   with `<<< TEXT` instead of a plain stdin; every other here-string caller
   in this file (jobctlSpawnForegroundHereString/jobctlRunBackgroundHereString
   below) is a thin wrapper passing -1/threadInvalidId, matching
   jobctlSpawnForeground/jobctlRunBackground's own relationship to
   jobctlSpawnPiped. Returns threadInvalidId on the same load/spawn
   failures as jobctlSpawnPiped, or if the pipe table itself is exhausted
   (vfsPipeCreateHandle, maxPipes slots all in use) -- both leave nothing
   spawned. */
uint32 jobctlSpawnPipedHereString(const char* path, const char* const* argv, int argc,
                                   const char* text, int stdoutHandle, uint32 joinPgid);

/* Same as jobctlSpawnForegroundRedirected, but for shell.c's new `NAME
   [ARGS] <<< TEXT [> OUT | >> OUT] [2> ERR | 2>> ERR]` here-string syntax
   instead of `< IN`: `text` (plus a trailing newline, matching a real
   shell's own `<<<` behavior) is fed directly into the child's fd 0
   through a real anonymous pipe (jobctlSpawnPipedHereString above) that
   THIS kernel thread itself writes and closes -- unlike jobctlApplyRedirects'
   inPath, there is no file opened at all, so `outPath`/`errPath` are still
   applied exactly as jobctlApplyRedirects already does (fd 0 is simply
   pre-wired to the here-string pipe instead of left at the console before
   that call runs). The write end is closed (real EOF, not a lingering open
   pipe) BEFORE this function ever returns, so by the time the child
   actually gets a timeslice to read its own fd 0 it sees exactly `text`
   followed by one newline followed by true end-of-file, with no
   possibility of racing the shell thread for more data that will never
   arrive. Returns threadInvalidId on the same load/spawn/redirect failures
   as jobctlSpawnForegroundRedirected, plus a pipe-table-exhaustion failure
   (vfsPipeCreateHandle, maxPipes slots all in use) -- both leave nothing
   spawned. */
uint32 jobctlSpawnForegroundHereString(const char* path, const char* const* argv, int argc,
                                        const char* text, const char* outPath, bool outAppend,
                                        const char* errPath, bool errAppend);

/* Same as jobctlRunBackgroundRedirected, but with the same here-string
   stdin support as jobctlSpawnForegroundHereString above -- shell.c's
   `NAME [ARGS] <<< TEXT [> OUT | >> OUT] [2> ERR | 2>> ERR] &`. Returns
   false (touching *outPid not at all) on the same load/spawn/redirect/
   pipe-exhaustion failures as jobctlSpawnForegroundHereString. */
bool jobctlRunBackgroundHereString(const char* path, const char* const* argv, int argc,
                                    const char* text, const char* outPath, bool outAppend,
                                    const char* errPath, bool errAppend,
                                    uint32* outPid);

/* Runs `path` to completion with its stdout wired to a real pipe THIS
   kernel thread reads back as a string -- shell.c's new `$(cmd)` command
   substitution, the mirror image of jobctlSpawnForegroundHereString's
   "write a fixed string into the child's stdin" above. stdin is left at
   its plain default (never redirected), and the terminal is never granted
   to the child (unlike every foreground job above) -- command substitution
   is not itself "the" foreground job, only a synchronous helper run while
   building one. Joins the child FIRST, then drains its captured stdout --
   not "read until EOF, then join": this kernel only ever releases a dead
   thread's own fds (reclaimOpenFds, sched.c) from inside threadReap
   (threadJoin's last step), never at threadExit/threadKill itself, so
   reading first would block forever waiting for an end-of-file that only
   the very threadJoin call coming after it could ever produce. Joining
   first instead means `path`'s own combined stdout must fit within one
   pipe buffer's worth (pipeBufferSize, pipe.h) without ever blocking on
   free space while it runs -- the same margin jobctlSpawnPipedHereString
   already relies on for a here-string write, applied here to a captured
   read instead -- an honestly-scoped limit on how chatty a substituted
   command may be, not a silent hang for anything that stays under it.
   Copies at most outCap-1 captured bytes into outBuf (NUL-terminated,
   truncating silently like every other fixed-capacity buffer in this
   kernel), draining and discarding anything beyond that rather than
   leaving it sitting in the pipe. *outStatus receives the child's real
   exit code. Returns false, touching neither outBuf nor *outStatus, on
   the same load/spawn/pipe-exhaustion failures as jobctlSpawnPiped/
   vfsPipeCreateHandle. */
bool jobctlCaptureOutput(const char* path, const char* const* argv, int argc,
                          char* outBuf, int outCap, int64* outStatus);

#endif
