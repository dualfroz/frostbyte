#include "jobctl.h"
#include "sched.h"
#include "vfs.h"
#include "fs.h"
#include "elf.h"
#include "paging.h"
#include "keyboard.h"
#include "pipe.h"
#include "string.h"

/* Matches kmain.c's userStackSize and syscall.c's execStackSize -- all
   three name the exact same "one slot's worth of stack" constant (elf.h's
   elfSlotStackOffset leaves 0x8000000 bytes below the stack top before the
   next thing in the slot, brk, starts; nothing in this kernel has ever
   needed anywhere near that much real stack). Kept as jobctl.c's own copy
   rather than sharing a header-level define with the other two call sites,
   the same "each caller of this pattern owns its own named copy" choice
   kmain.c and syscall.c already made independently of each other. */
#define jobctlStackSize 0x4000

/* Shared by both functions below: resolves `path` through the VFS, loads
   it at its own fixed ELF-linked slot (elfLoad picks the address, same as
   syscall.c's loadImageByPath), and spawns it as a brand new process group
   leader -- the one behavioral difference from doSpawn (syscall.c), which
   leaves a freshly spawned child in its parent's existing group instead.
   Returns threadInvalidId, having created nothing, on any failure (missing
   binary, no execute permission, bad ELF, stack mapping failure, or a full
   thread table). */
static uint32 jobctlSpawn(const char* path, const char* const* argv, int argc)
{
    VfsNode* node = vfsLookup(path);
    if (!node)
        return threadInvalidId;
    /* Real EACCES-shaped refusal, same check and same reasoning as
       syscall.c's loadImageByPath (sysExec/sysSpawn's own parallel loading
       path) -- a file the CALLING thread's identity is not permitted to
       exec (vfsPermitted, owner-vs-other-vs-root) can never be spawned this
       way either. Collapsed into the same undifferentiated threadInvalidId
       failure as every other reason jobctlSpawn already refuses (missing
       binary, bad ELF, ...), matching this function's own existing doc
       comment. */
    if (!vfsPermitted(vfsMode(node), vfsUid(node), vfsGid(node), currentThreadUid(), currentThreadGid(), fsModeOwnerExec))
        return threadInvalidId;

    const void* image = vfsFileData(node);
    uint64 size = vfsSize(node);
    uint64 entry = elfLoad(image, size);
    if (entry == 0)
        return threadInvalidId;

    uint64 stackTop = elfSlotStackTop(entry);
    if (!pagingMapUserRangeFlags(stackTop - jobctlStackSize, jobctlStackSize, pageNoExecute))
        return threadInvalidId;

    uint64 initialRsp;
    if (!elfBuildInitialStack(stackTop, jobctlStackSize, argv, argc, &initialRsp))
        return threadInvalidId;

    uint32 pid = threadCreateUser(path, entry, initialRsp);
    if (pid == threadInvalidId)
        return threadInvalidId;

    threadSetBrk(pid, elfSlotBrkStart(entry));

    /* A real shell's fork()+setpgid(child, child): every job started
       through jobctl gets its own fresh group regardless of the caller's
       own pgid, so a later Ctrl-C aimed at THIS job (via
       keyboardSetForegroundPgid below) can never also reach some earlier,
       unrelated job that happened to share the caller's group. */
    threadSetPgid(pid, pid);
    return pid;
}

uint32 jobctlSpawnPiped(const char* path, const char* const* argv, int argc,
                         int stdinHandle, int stdoutHandle, uint32 joinPgid)
{
    uint32 pid = jobctlSpawn(path, argv, argc);
    if (pid == threadInvalidId)
        return threadInvalidId;

    /* jobctlSpawn above already made this its own new group leader --
       overridden here, after the fact, only for a pipeline's second-and-
       later stage, which needs to join the FIRST stage's group instead so
       the two are scoped together for signals (threadKillGroup/
       sigRaiseGroup, sched.h/signal.h) exactly like a real shell's
       fork()+setpgid(child, leaderPid) for every stage after the first. */
    if (joinPgid != threadInvalidId)
        threadSetPgid(pid, joinPgid);

    /* Bakes the requested pipe ends into fd 0/fd 1 of a thread that has not
       run a single instruction yet (threadCreateUser, inside jobctlSpawn
       above, already set its state Runnable -- see that function's own
       field-by-field ordering -- but nothing yields between here and
       jobctlSpawn's return, the same timing this file's own
       threadSetBrk/threadSetPgid calls already rely on). fd 0's default
       console reader (vfsFdInit, vfs.c) is released first so redirecting it
       never leaks that reader slot -- exactly the resource reclaimOpenFds
       (sched.c) already frees for a thread that dies still holding it,
       just done here up front instead of at exit. fd 1 has no such
       resource to release: its own default (fdConsole, no reader) owns
       nothing. */
    FileDescriptor* fds = threadAt(pid)->fds;
    if (stdinHandle >= 0) {
        keyboardCloseReader(fds[0].pipeHandle);
        vfsPipeAttachInto(fds, 0, stdinHandle, false);
    }
    if (stdoutHandle >= 0)
        vfsPipeAttachInto(fds, 1, stdoutHandle, true);

    return pid;
}

uint32 jobctlSpawnForeground(const char* path, const char* const* argv, int argc)
{
    uint32 pid = jobctlSpawnPiped(path, argv, argc, -1, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return threadInvalidId;

    keyboardSetForegroundPgid(pid);
    return pid;
}

int64 jobctlWaitForeground(uint32 pid)
{
    /* Read BEFORE threadJoin, not after: this is the WAITER's own group
       (the shell, still very much alive), not the job's -- reading it
       after threadJoin would still be correct today since nothing else
       changes the calling thread's own pgid in between, but reading it
       first keeps this function correct even if that ever changes, and
       costs nothing. */
    uint32 callerPgid = threadPgid(currentThreadId());

    int64 code = threadJoin(pid);

    /* Reclaim the terminal for the CALLER's own group the instant the job
       exits -- deliberately not "whatever pgid happened to own the
       terminal right before jobctlSpawnForeground ran" (which could itself
       be some earlier, now-dead job's stale id if this is the very first
       foreground job the caller has ever run): a real shell always gets
       its own prompt's terminal back when a foreground job quits, so a
       later Ctrl-C typed at the prompt reaches the shell's NEXT job, not a
       group nothing will ever answer for again. */
    keyboardSetForegroundPgid(callerPgid);

    return code;
}

int64 jobctlWaitPipelineStages(const uint32* pids, int count)
{
    /* Same "read before threadJoin" reasoning as jobctlWaitForeground
       above. */
    uint32 callerPgid = threadPgid(currentThreadId());

    /* Grants the terminal to the pipeline's shared group (pids[0] IS that
       group's id -- jobctlSpawnPiped's own joinPgid contract already made
       every other pid a member of it) once, for the whole chain together,
       rather than per-spawn -- a Ctrl-C typed while the pipeline runs
       reaches every stage at once, exactly like jobctlSpawnForeground
       already does for a single job. */
    keyboardSetForegroundPgid(pids[0]);

    /* threadJoin blocks on a SPECIFIC pid regardless of which stage
       actually exits first -- joining every one, in this fixed order,
       reaps all of them correctly no matter the real completion order, the
       same way a real shell's own wait() for each of a pipeline's known
       children would. */
    int64 lastCode = 0;
    for (int i = 0; i < count; i++)
        lastCode = threadJoin(pids[i]);

    keyboardSetForegroundPgid(callerPgid);

    return lastCode;
}

bool jobctlRunBackground(const char* path, const char* const* argv, int argc,
                          uint32* outPid)
{
    uint32 pid = jobctlSpawnPiped(path, argv, argc, -1, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return false;

    /* Deliberately no keyboardSetForegroundPgid call at all -- a
       backgrounded job never owns the terminal, so a Ctrl-C typed while it
       runs reaches whatever WAS already in the foreground instead (the
       shell itself, if nothing else is running in the foreground), never
       this job. */
    *outPid = pid;
    return true;
}

bool jobctlApplyRedirects(uint32 pid, const char* inPath, const char* outPath, bool outAppend,
                           const char* errPath, bool errAppend)
{
    FileDescriptor* fds = threadAt(pid)->fds;

    if (inPath) {
        /* Same "release the default console reader before overwriting fd 0"
           step jobctlSpawnPiped already does for a piped stdin above --
           keyboardCloseReader is a safe no-op on an already-closed/invalid
           handle (readerAt's own bounds+used check, keyboard.c), so this
           is correct even if fd 0 already holds something else entirely
           (e.g. a pipe end from an earlier jobctlSpawnPiped call on this
           same pid, in runPipeline's left-side case below). */
        keyboardCloseReader(fds[0].pipeHandle);
        if (vfsOpenInto(fds, 0, inPath, oReadOnly) < 0)
            return false;
    }

    if (outPath) {
        uint32 flags = oCreate | oWriteOnly | (outAppend ? oAppend : oTrunc);
        if (vfsOpenInto(fds, 1, outPath, flags) < 0)
            return false;
    }

    if (errPath) {
        uint32 flags = oCreate | oWriteOnly | (errAppend ? oAppend : oTrunc);
        if (vfsOpenInto(fds, 2, errPath, flags) < 0)
            return false;
    }

    return true;
}

uint32 jobctlSpawnForegroundRedirected(const char* path, const char* const* argv, int argc,
                                        const char* inPath, const char* outPath, bool outAppend,
                                        const char* errPath, bool errAppend)
{
    uint32 pid = jobctlSpawnPiped(path, argv, argc, -1, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return threadInvalidId;

    if (!jobctlApplyRedirects(pid, inPath, outPath, outAppend, errPath, errAppend)) {
        threadKill(pid);
        threadJoin(pid);
        return threadInvalidId;
    }

    keyboardSetForegroundPgid(pid);
    return pid;
}

bool jobctlRunBackgroundRedirected(const char* path, const char* const* argv, int argc,
                                    const char* inPath, const char* outPath, bool outAppend,
                                    const char* errPath, bool errAppend,
                                    uint32* outPid)
{
    uint32 pid = jobctlSpawnPiped(path, argv, argc, -1, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return false;

    if (!jobctlApplyRedirects(pid, inPath, outPath, outAppend, errPath, errAppend)) {
        threadKill(pid);
        threadJoin(pid);
        return false;
    }

    *outPid = pid;
    return true;
}

/* Generalizes jobctlSpawnPiped's own fd-baking-before-first-instruction
   machinery to also here-string-feed stdin: spawns `path` with fd 0 wired
   to a brand-new pipe AND fd 1/pgid wired exactly as `stdoutHandle`/
   `joinPgid` ask (passed straight through to jobctlSpawnPiped, unlike the
   fixed -1/threadInvalidId every earlier here-string caller in this file
   used), then feeds `text` plus a trailing newline into the new pipe's
   write end and closes it -- all BEFORE returning, so the child can never
   observe the write end still open. Ordering matters: the write end is
   only added/used/closed AFTER jobctlSpawnPiped has already attached the
   read end (pipeAddReader, via vfsPipeAttachInto in vfs.c) -- pipe.c's
   pipeReleaseIfUnused only frees a pipe's slot once BOTH readers and
   writers reach 0, so closing the write end while the read end is still
   attached (readers==1) leaves the pipe, and the data already sitting in
   its ring buffer, alive for the child to actually read; closing it
   BEFORE the read end attached (readers==0) would instead free the whole
   slot out from under the still-buffered payload the instant this
   function's own pipeCloseWrite ran, handing the child a dangling handle.
   `text`'s own length plus one newline byte is always far under
   pipeBufferSize (256, pipe.h) for any line this shell's own lineCapacity
   (128, shell.c) could ever have typed it from, so the non-blocking
   pipeWrite below can never see "buffer full" and needs no retry loop the
   way a general-purpose writer would. Returns threadInvalidId, having
   spawned and written nothing, on the same load/spawn failures as
   jobctlSpawnPiped, or if the pipe table itself is exhausted
   (vfsPipeCreateHandle). */
uint32 jobctlSpawnPipedHereString(const char* path, const char* const* argv, int argc,
                                   const char* text, int stdoutHandle, uint32 joinPgid)
{
    int handle = vfsPipeCreateHandle();
    if (handle < 0)
        return threadInvalidId;

    uint32 pid = jobctlSpawnPiped(path, argv, argc, handle, stdoutHandle, joinPgid);
    if (pid == threadInvalidId) {
        pipeDestroyIfUnattached(handle);
        return threadInvalidId;
    }

    pipeAddWriter(handle);
    uint64 length = strlen(text);
    if (length > 0)
        pipeWrite(handle, text, length);
    pipeWrite(handle, "\n", 1);
    pipeCloseWrite(handle);

    return pid;
}

uint32 jobctlSpawnForegroundHereString(const char* path, const char* const* argv, int argc,
                                        const char* text, const char* outPath, bool outAppend,
                                        const char* errPath, bool errAppend)
{
    uint32 pid = jobctlSpawnPipedHereString(path, argv, argc, text, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return threadInvalidId;

    /* inPath is always nullptr here: fd 0 is already the here-string pipe
       jobctlSpawnPipedHereString just wired up, exactly like a piped
       stdin's slot in runPipeline (shell.c) is never ALSO given a file
       redirect on the same fd. */
    if (!jobctlApplyRedirects(pid, nullptr, outPath, outAppend, errPath, errAppend)) {
        threadKill(pid);
        threadJoin(pid);
        return threadInvalidId;
    }

    keyboardSetForegroundPgid(pid);
    return pid;
}

bool jobctlRunBackgroundHereString(const char* path, const char* const* argv, int argc,
                                    const char* text, const char* outPath, bool outAppend,
                                    const char* errPath, bool errAppend,
                                    uint32* outPid)
{
    uint32 pid = jobctlSpawnPipedHereString(path, argv, argc, text, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return false;

    if (!jobctlApplyRedirects(pid, nullptr, outPath, outAppend, errPath, errAppend)) {
        threadKill(pid);
        threadJoin(pid);
        return false;
    }

    *outPid = pid;
    return true;
}

/* The mirror image of jobctlSpawnPipedHereString above: that one feeds a
   fixed string INTO a freshly spawned child's stdin (the shell is the
   writer), this one reads a freshly spawned child's stdout back OUT as a
   string (the shell is the reader) -- shell.c's new `$(cmd)` command
   substitution. Attaches this thread as the pipe's reader (pipeAddReader)
   BEFORE jobctlSpawnPiped ever attaches the child as its writer, the exact
   opposite order of jobctlSpawnPipedHereString's own writer-after-reader
   ordering but for the same reason: whichever end never gets attached at
   all must not let pipeCloseRead/pipeCloseWrite release the slot out from
   under the other end while it is still in use. stdin is left at its plain
   default (whatever fd 0 already is for this kernel thread, exactly like
   jobctlRunBackground's own unpiped stdin) -- an honestly-scoped limitation
   matching runPipelineStages' "no stage may be a builtin": a substituted
   command that genuinely needs to read stdin itself has nothing useful
   wired to it here, since command substitution has no argv-supplied input
   of its own to give it.
   Joins the child (threadJoin) BEFORE ever reading a single byte, then
   drains whatever it wrote with plain non-blocking pipeRead calls -- the
   opposite order of "read until EOF, then join" would deadlock in this
   kernel every time: reclaimOpenFds (sched.c), which closes a dead
   thread's own fd 1 and so is what actually makes pipeRead ever report
   real end-of-file (writers reaching 0), runs ONLY from inside threadReap
   (threadJoin's own last step), never from threadExit/threadKill
   themselves -- a thread that has already died but not yet been reaped
   still counts, from the pipe's own point of view, as holding fd 1 open.
   Reading first would therefore block forever waiting for an EOF that can
   only ever arrive AFTER the very threadJoin call that would come next.
   Joining first instead means the child's own write(s) must already have
   completed (and fit in the pipe's ring buffer without blocking on free
   space, exactly like jobctlSpawnPipedHereString's own here-string write
   already assumes for the identical pipeBufferSize=256 constant) by the
   time it actually exits -- an honestly-scoped limitation this function
   shares with that one: a substituted command whose own combined stdout
   exceeds pipeBufferSize bytes would block on a full buffer waiting for a
   reader that will not start draining until this function's own
   threadJoin below returns, which itself requires the write to unblock
   first -- the same ordering deadlock in the opposite direction, avoided
   here simply by staying within the same generous margin every other
   pipeBufferSize-sized fixed buffer in this kernel already relies on.
   Copies at most outCap-1 bytes into outBuf (NUL-terminated); any further
   buffered bytes past that are still drained (into a small scratch
   buffer, discarded) rather than left sitting in the pipe. *outStatus
   receives the child's real exit code. Returns false, touching neither
   outBuf nor *outStatus, on the same load/spawn failures as
   jobctlSpawnPiped, or if the pipe table itself is exhausted
   (vfsPipeCreateHandle). */
bool jobctlCaptureOutput(const char* path, const char* const* argv, int argc,
                          char* outBuf, int outCap, int64* outStatus)
{
    int handle = vfsPipeCreateHandle();
    if (handle < 0)
        return false;

    pipeAddReader(handle);

    uint32 pid = jobctlSpawnPiped(path, argv, argc, -1, handle, threadInvalidId);
    if (pid == threadInvalidId) {
        pipeCloseRead(handle);
        return false;
    }

    *outStatus = threadJoin(pid);

    int used = 0;
    char scratch[64];
    for (;;) {
        int64 n = (used < outCap - 1)
            ? pipeRead(handle, outBuf + used, (uint64)(outCap - 1 - used))
            : pipeRead(handle, scratch, sizeof(scratch));
        if (n <= 0)
            break;
        if (used < outCap - 1)
            used += (int)n;
    }
    outBuf[used] = 0;

    pipeCloseRead(handle);
    return true;
}
