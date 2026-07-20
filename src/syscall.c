#include "syscall.h"
#include "isr.h"
#include "console.h"
#include "sched.h"
#include "vfs.h"
#include "fs.h"
#include "string.h"
#include "elf.h"
#include "paging.h"
#include "signal.h"
#include "keyboard.h"
#include "pit.h"
#include "poll.h"
#include "rtc.h"
#include "mmap.h"

#define userSpaceMin 0x0000000040000000ull
#define userSpaceMax 0x0000800000000000ull

/* Matches kmain.c's userStackSize -- both sides map exactly one slot's
   worth of stack for a freshly loaded binary, whether it was spawned at
   boot (kmain.c's spawnUser, addresses picked by hand) or at runtime
   (doSpawn/doExec below, addresses derived from elfSlotStackTop). */
#define execStackSize 0x4000

static bool userRangeOk(uint64 pointer, uint64 length)
{
    if (pointer < userSpaceMin)
        return false;
    if (pointer + length < pointer)
        return false;
    if (pointer + length > userSpaceMax)
        return false;
    return true;
}

static bool copyUserPath(uint64 pointer, char* dst, uint64 max)
{
    if (pointer < userSpaceMin || pointer >= userSpaceMax)
        return false;
    const char* src = (const char*)pointer;
    for (uint64 i = 0; i < max; i++) {
        dst[i] = src[i];
        if (src[i] == 0)
            return true;
    }
    dst[max - 1] = 0;
    return true;
}

#define execArgMaxLen 256

/* Copies a NULL-terminated array of user-space `const char*` (the argv
   parameter of sysExec/sysSpawn -- userExec/userSpawn, ulib.c) into kernel
   memory: up to elfMaxArgs (elf.h) pointers are read out of userspace one
   at a time (each individually range-checked, since the array itself is
   just user memory a buggy or hostile caller controls) and each string
   copied via copyUserPath into `storage`, which the caller owns for as
   long as it needs argvPtrs to stay valid. A null `argvPointer` (the
   common "no arguments" case) is not an error -- it yields argc 0, letting
   userExec(path, nullptr) mean the same as a bare execve(path) with an
   otherwise-empty argv. Fails (leaving *outArgc untouched) on a bad
   pointer, a string that doesn't fit `execArgMaxLen`, or an argv longer
   than elfMaxArgs entries that never hits its NULL terminator. */
static bool copyUserArgv(uint64 argvPointer, char storage[elfMaxArgs][execArgMaxLen],
                          const char* argvPtrs[elfMaxArgs], int* outArgc)
{
    if (argvPointer == 0) {
        *outArgc = 0;
        return true;
    }
    if (!userRangeOk(argvPointer, sizeof(uint64) * (elfMaxArgs + 1)))
        return false;

    const uint64* userArgv = (const uint64*)argvPointer;
    int argc = 0;
    while (argc < elfMaxArgs && userArgv[argc] != 0) {
        if (!copyUserPath(userArgv[argc], storage[argc], execArgMaxLen))
            return false;
        argvPtrs[argc] = storage[argc];
        argc++;
    }
    if (argc == elfMaxArgs && userArgv[argc] != 0)
        return false;

    *outArgc = argc;
    return true;
}

static int64 doWrite(uint64 fd, uint64 buffer, uint64 length)
{
    if (!userRangeOk(buffer, length))
        return -1;
    return vfsWrite((int)fd, (const void*)buffer, length);
}

static int64 doRead(uint64 fd, uint64 buffer, uint64 length)
{
    if (!userRangeOk(buffer, length))
        return -1;
    return vfsRead((int)fd, (void*)buffer, length);
}

typedef struct LoadedImage {
    uint64 entry;
    uint64 stackTop;
    uint64 brkStart;
} LoadedImage;

/* Shared by doExec and doSpawn: resolves `path` through the VFS, loads its
   LOAD segments via elfLoad (which maps them at whatever fixed virtual
   address they were linked at -- see elf.c), then derives and maps a fresh
   stack for that slot from nothing but the ELF's own entry point (see
   elfSlotStackTop/elf.h). Leaves the caller's thread state untouched;
   callers decide what to do with a successfully loaded image. */
static bool loadImageByPath(const char* path, LoadedImage* out)
{
    VfsNode* node = vfsLookup(path);
    if (!node)
        return false;
    /* Real EACCES-shaped refusal: a file without the right exec bit for the
       CALLING thread's identity (vfsPermitted, owner-vs-other-vs-root) can
       never be exec'd or spawned, exactly like real Unix's own execve()
       check -- jobctl.c's own jobctlSpawn (the shell's separate, parallel
       loading path) enforces the identical check the identical way. */
    if (!vfsPermitted(vfsMode(node), vfsUid(node), vfsGid(node), currentThreadUid(), currentThreadGid(), fsModeOwnerExec))
        return false;

    const void* image = vfsFileData(node);
    uint64 size = vfsSize(node);
    uint64 entry = elfLoad(image, size);
    if (entry == 0)
        return false;

    out->entry = entry;
    out->stackTop = elfSlotStackTop(entry);
    out->brkStart = elfSlotBrkStart(entry);

    return pagingMapUserRangeFlags(out->stackTop - execStackSize, execStackSize, pageNoExecute);
}

/* execve()-lite: replaces the calling thread's own program image with a
   different one, in place -- same pid, same open file descriptors (POSIX's
   default, no O_CLOEXEC support to opt out of here), new code/stack/brk.
   Mutates the live interrupt frame's rip/rsp directly (the same trick
   threadCreateUser uses to bootstrap a brand new thread's very first
   frame): on success this syscall does not "return" to the caller in any
   meaningful sense, since regs->rip now points at the new image's entry
   point instead of back into the old one's code -- exactly real exec()
   semantics. General-purpose registers are zeroed so the new program never
   observes leftover state from a syscall it never made. Failure (bad path,
   not a valid ELF, thread table/frame mapping issue) leaves regs untouched
   apart from the -1 return value: the caller keeps running its old image,
   also matching real exec().

   Also reclaims the OLD image's slot: frostbyte's flat single-address-space
   design (see doSpawn below) means a program that exec()s into a binary
   linked at a different fixed base -- the common case, since two distinct
   binaries can never share a slot -- would otherwise leak its entire
   elfSlotSize worth of physical frames every time, forever. Skipped only
   when the new image happens to land in the SAME slot as the old one (a
   binary exec()ing itself): unmapping first would free frames the new
   image's own loadImageByPath call had already mapped moments earlier.

   argv/argc are already kernel-side copies (copyUserArgv, called from the
   sysExec dispatch case below, BEFORE this runs) -- critical ordering,
   since they may point into the OLD image's own memory (a process's argv
   commonly lives in its own .rodata/stack), which this function is about
   to unmap out from under it. */
static bool doExec(const char* path, const char* const* argv, int argc, Registers* regs)
{
    uint32 pid = currentThreadId();
    uint64 oldSlotBase = threadUserSlotBase(pid);

    LoadedImage img;
    if (!loadImageByPath(path, &img))
        return false;

    uint64 initialRsp;
    if (!elfBuildInitialStack(img.stackTop, execStackSize, argv, argc, &initialRsp))
        return false;

    uint64 newSlotBase = elfSlotBase(img.entry);
    if (oldSlotBase != 0 && oldSlotBase != newSlotBase) {
        uint64 stackProbe = oldSlotBase + elfSlotStackOffset - 1;
        bool wasMapped = pagingResolve(stackProbe) != 0;

        pagingUnmapUserRange(oldSlotBase, elfSlotSize);

        bool nowFreed = pagingResolve(stackProbe) == 0;
        kprintf("[exec] pid=%u freed old slot %p (was-mapped=%s now-freed=%s) -> %s\n",
                pid, (void*)oldSlotBase, wasMapped ? "yes" : "no", nowFreed ? "yes" : "no",
                (wasMapped && nowFreed) ? "exec-old-slot-freed-ok" : "exec-old-slot-freed-FAIL");
    } else if (oldSlotBase != 0) {
        /* Self-exec (or the pid==0/no-prior-slot case, unreachable for a
           real ring-3 caller): the branch above is skipped precisely to
           avoid unmapping the NEW image's own LOAD segments/stack, which
           already landed on these same pages -- but any mmap mapping from
           the OLD image is a disjoint sub-range (mmap.h's own mmapReserve
           region, [slot+elfSlotStackOffset, slot+elfSlotSize)) that
           loadImageByPath/elfBuildInitialStack never touch, so it still
           needs reclaiming here explicitly or its frames would leak
           forever every time a binary exec()s itself. */
        pagingUnmapUserRange(oldSlotBase + elfSlotStackOffset, elfSlotSize - elfSlotStackOffset);
    }
    if (oldSlotBase != 0)
        mmapReleaseAll(threadAt(pid));
    threadSetUserSlotBase(pid, newSlotBase);

    threadSetName(pid, path);
    threadSetBrk(pid, img.brkStart);

    regs->rip = img.entry;
    regs->rsp = initialRsp;
    regs->rbp = 0;
    regs->rdi = 0;
    regs->rsi = 0;
    regs->rdx = 0;
    regs->rcx = 0;
    regs->rbx = 0;
    regs->rax = 0;
    regs->r8 = 0;
    regs->r9 = 0;
    regs->r10 = 0;
    regs->r11 = 0;
    regs->r12 = 0;
    regs->r13 = 0;
    regs->r14 = 0;
    regs->r15 = 0;
    return true;
}

/* posix_spawn()-lite: loads `path` into a brand new thread/process rather
   than replacing the caller, and returns its pid immediately without
   waiting -- the caller joins it later with sysWait (threadJoin). This is
   deliberately not a literal fork(): frostbyte has one shared address space
   system-wide (no per-process page tables/CR3, see paging.c), so there is
   no address space to duplicate, only a fresh slot to load a binary into.
   As with any binary in this kernel, spawning two live instances of the
   same path (or of two paths whose fixed link addresses happen to share a
   slot) at once would map both onto the same pages and corrupt each other;
   this is an accepted limitation of the flat single-address-space design,
   not something doSpawn can detect from a path string alone. */
static int64 doSpawn(const char* path, const char* const* argv, int argc)
{
    LoadedImage img;
    if (!loadImageByPath(path, &img))
        return -1;

    uint64 initialRsp;
    if (!elfBuildInitialStack(img.stackTop, execStackSize, argv, argc, &initialRsp))
        return -1;

    uint32 pid = threadCreateUser(path, img.entry, initialRsp);
    if (pid == threadInvalidId)
        return -1;

    threadSetBrk(pid, img.brkStart);
    return (int64)pid;
}

uint64 syscallDispatch(Registers* regs)
{
    /* This thread is not (yet, or any more) parked mid a retryable
       syscall's own block-and-retry pattern the instant ANY dispatch for
       it begins -- whether this dispatch turns out to BE that exact retry
       re-executing (sysRead/sysWrite/sysWait's own cases set it true again
       below if they block once more) or a completely unrelated syscall.
       See sched.h's Thread.sigInRetryableSyscall and signal.c's
       sigConsumePending for why this precise a window matters: without it,
       a signal delivered while this thread is doing perfectly ordinary
       work could be mistaken for one interrupting a slow syscall. */
    currentThread()->sigInRetryableSyscall = false;

    switch (regs->rax) {
    case sysWrite: {
        int64 result = doWrite(regs->rdi, regs->rsi, regs->rdx);
        if (result == -1) {
            /* A "would block" result on a fdFlagBlocking pipe end (see
               vfs.h) is retried, not returned: park on the pipe's channel
               and rewind rip by the 2-byte `int $0x80` this trap came from
               so the whole syscall re-executes once schedWakeChannel makes
               this thread Runnable again (pipeWrite, called from doWrite
               above, already does that whenever it frees or fills space) --
               the exact restart-syscall pattern sysWait uses for
               threadJoin, for the same reason: halt()ing here directly
               would freeze the whole machine, not just this thread, since
               this handler runs with interrupts disabled. A non-blocking
               pipe end (vfsPipe, e.g. pipetest.c) or a permanently broken
               one gets nullptr back and the -1 passes through untouched. */
            const void* channel = vfsBlockingChannel((int)regs->rdi, true);
            if (channel) {
                currentThread()->sigInRetryableSyscall = true;
                schedBlockCurrent(channel);
                regs->rip -= 2;
                return schedule((uint64)regs);
            }
        }
        regs->rax = (uint64)result;
        return (uint64)regs;
    }
    case sysRead: {
        int64 result = doRead(regs->rdi, regs->rsi, regs->rdx);
        if (result == -1) {
            /* Covers a fdFlagBlocking pipe/fifo end (see sysWrite above) and
               fd 0's console reader queue (keyboard.h) alike -- both report
               "would block" as -1 from doRead, and vfsBlockingChannel picks
               the right wait channel for whichever kind fd actually is. */
            const void* channel = vfsBlockingChannel((int)regs->rdi, false);
            if (channel) {
                currentThread()->sigInRetryableSyscall = true;
                schedBlockCurrent(channel);
                regs->rip -= 2;
                return schedule((uint64)regs);
            }
        }
        regs->rax = (uint64)result;
        return (uint64)regs;
    }
    case sysGetpid:
        regs->rax = currentThreadId();
        return (uint64)regs;
    case sysSbrk:
        regs->rax = currentThreadSbrk((int64)regs->rdi);
        return (uint64)regs;
    case sysYield:
        regs->rax = 0;
        return schedule((uint64)regs);
    case sysSleep:
        threadSleep(regs->rdi);
        regs->rax = 0;
        return schedule((uint64)regs);
    case sysExit:
        threadExit(regs->rdi);
        return schedule((uint64)regs);
    case sysOpen: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsOpen(path, (uint32)regs->rsi);
        return (uint64)regs;
    }
    case sysClose:
        regs->rax = (uint64)(int64)vfsClose((int)regs->rdi);
        return (uint64)regs;
    case sysLseek:
        regs->rax = (uint64)vfsLseek((int)regs->rdi, (int64)regs->rsi, (int)regs->rdx);
        return (uint64)regs;
    case sysStat: {
        char path[256];
        FbStat st;
        if (copyUserPath(regs->rdi, path, sizeof(path)) &&
            userRangeOk(regs->rsi, sizeof(FbStat)) &&
            vfsStat(path, &st) == 0) {
            memcpy((void*)regs->rsi, &st, sizeof(FbStat));
            regs->rax = 0;
        } else {
            regs->rax = (uint64)-1;
        }
        return (uint64)regs;
    }
    case sysGetdents:
        if (!userRangeOk(regs->rsi, regs->rdx))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)vfsGetdents((int)regs->rdi, (void*)regs->rsi, regs->rdx);
        return (uint64)regs;
    case sysMkdir: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsMkdir(path);
        return (uint64)regs;
    }
    case sysChdir: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsChdir(path);
        return (uint64)regs;
    }
    case sysGetcwd:
        if (!userRangeOk(regs->rdi, regs->rsi))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)vfsGetcwd((char*)regs->rdi, regs->rsi);
        return (uint64)regs;
    case sysGetEnv: {
        char name[envEntryMax];
        if (!copyUserPath(regs->rdi, name, sizeof(name)) || !userRangeOk(regs->rsi, regs->rdx)) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        const char* value = currentThreadGetEnv(name);
        if (!value) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        uint64 len = strlen(value);
        if (len >= regs->rdx) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        memcpy((void*)regs->rsi, value, len + 1);
        regs->rax = (uint64)len;
        return (uint64)regs;
    }
    case sysSetEnv: {
        char name[envEntryMax];
        char value[envEntryMax];
        if (!copyUserPath(regs->rdi, name, sizeof(name)) ||
            !copyUserPath(regs->rsi, value, sizeof(value))) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        regs->rax = currentThreadSetEnv(name, value) ? 0 : (uint64)-1;
        return (uint64)regs;
    }
    case sysUnsetEnv: {
        /* Real unsetenv(3): a no-op success whether or not `name` actually
           had an entry -- ONLY a bad user pointer/path is a real failure
           here, unlike sysSetEnv above where "table full"/"too long" are
           genuine failures. currentThreadUnsetEnv's own bool return (which
           DOES distinguish "removed" from "was not there", for a kernel-
           side caller like shell.c that might want to) is deliberately
           discarded at this syscall boundary. */
        char name[envEntryMax];
        if (!copyUserPath(regs->rdi, name, sizeof(name))) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        currentThreadUnsetEnv(name);
        regs->rax = 0;
        return (uint64)regs;
    }
    case sysUnlink: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsUnlink(path);
        return (uint64)regs;
    }
    case sysRmdir: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsRmdir(path);
        return (uint64)regs;
    }
    case sysChmod: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsChmod(path, (uint32)regs->rsi);
        return (uint64)regs;
    }
    case sysChown: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsChown(path, (uint32)regs->rsi);
        return (uint64)regs;
    }
    case sysGetuid:
        /* Real getuid(2): no arguments, always succeeds, straight through
           to the calling thread's own credential. */
        regs->rax = (uint64)currentThreadUid();
        return (uint64)regs;
    case sysSetuid:
        /* Real setuid(2): currentThreadSetUid (sched.h) itself enforces the
           actual privilege rule (uid 0 may become anyone; anyone else may
           only pass their own current uid back in) -- this dispatch case
           does no additional validation of its own, matching sysChdir's own
           "the underlying function already checks everything" split. */
        regs->rax = currentThreadSetUid((uint32)regs->rdi) ? 0 : (uint64)-1;
        return (uint64)regs;
    case sysGetgid:
        /* Real getgid(2): same shape as sysGetuid above. */
        regs->rax = (uint64)currentThreadGid();
        return (uint64)regs;
    case sysSetgid:
        /* Real setgid(2): currentThreadSetGid (sched.h) itself enforces the
           actual privilege rule (gated by this thread's own UID, not GID --
           see that function's own doc comment). */
        regs->rax = currentThreadSetGid((uint32)regs->rdi) ? 0 : (uint64)-1;
        return (uint64)regs;
    case sysChgrp: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsChgrp(path, (uint32)regs->rsi);
        return (uint64)regs;
    }
    case sysPidfdOpen:
        /* vfsPidfdOpen (vfs.c) already does every validity check pidfd.h's
           own pidfdOpen needs (id 0/out-of-range/already-unused target) and
           the fd-table installation vfsPipe/vfsPipeAttach's own dispatch
           cases above delegate the same way -- nothing left for this
           handler to add. */
        regs->rax = (uint64)vfsPidfdOpen((uint32)regs->rdi);
        return (uint64)regs;
    case sysPidfdSend: {
        /* vfsPidfdTarget folds "is this fd even a live pidfd" and "has the
           original target since been reaped/recycled" (pidfd.h's own
           generation check) into one call -- a stale handle is rejected
           right here, before sigRaiseInfo ever sees a pid, which is
           exactly the ABA-safety pidfd.h exists to provide over a raw
           userSigSend(pid, signum) on a cached pid (see pidfd.h's own file
           comment). Otherwise identical to sysSigSend's own single-target
           path just above (same siCodeUser/senderPid convention, same
           sigKill/sigStop bypasses already living inside sigRaiseInfo). */
        uint32 target = vfsPidfdTarget((int)regs->rdi);
        if (target == threadInvalidId) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        int signum = (int)regs->rsi;
        bool ok = sigRaiseInfo(target, signum, siCodeUser, (int)currentThreadId(), 0);
        regs->rax = ok ? 0 : (uint64)-1;
        return (uint64)regs;
    }
    case sysPidfdWait: {
        /* Same retry-the-syscall park pattern as sysWait's own specific-pid
           path below, just resolving the target through a pidfd instead of
           a raw pid -- re-resolved on EVERY retry (not just the first
           check), so a target that gets reaped by someone else while this
           thread is blocked is caught the instant this thread wakes back
           up, rather than only at the moment sysPidfdWait was first called.
           threadJoin(target) below only ever reaps a thread this exact
           check just proved is threadDead AND still the same incarnation
           this handle was opened on, so there is no risk of it silently
           reaping (or reporting the exit code of) an unrelated thread that
           happens to have landed on the same numeric slot since. */
        uint32 target = vfsPidfdTarget((int)regs->rdi);
        if (target == threadInvalidId) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        Thread* targetThread = threadAt(target);
        if (targetThread->state != threadDead) {
            currentThread()->sigInRetryableSyscall = true;
            schedBlockCurrent(targetThread);
            regs->rip -= 2;
            return schedule((uint64)regs);
        }
        regs->rax = (uint64)threadJoin(target);
        return (uint64)regs;
    }
    case sysSymlink: {
        char target[256];
        char path[256];
        if (!copyUserPath(regs->rdi, target, sizeof(target)) ||
            !copyUserPath(regs->rsi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsSymlink(path, target);
        return (uint64)regs;
    }
    case sysReadlink: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)) || !userRangeOk(regs->rsi, regs->rdx))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)vfsReadlink(path, (char*)regs->rsi, regs->rdx);
        return (uint64)regs;
    }
    case sysLink: {
        char oldPath[256];
        char newPath[256];
        if (!copyUserPath(regs->rdi, oldPath, sizeof(oldPath)) ||
            !copyUserPath(regs->rsi, newPath, sizeof(newPath)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsLink(oldPath, newPath);
        return (uint64)regs;
    }
    case sysPipe: {
        if (!userRangeOk(regs->rdi, 2 * sizeof(int))) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        int* userFds = (int*)regs->rdi;
        int readFd, writeFd;
        if (vfsPipe(&readFd, &writeFd) < 0) {
            regs->rax = (uint64)-1;
        } else {
            userFds[0] = readFd;
            userFds[1] = writeFd;
            regs->rax = 0;
        }
        return (uint64)regs;
    }
    case sysPipeCreate:
        regs->rax = (uint64)vfsPipeCreateHandle();
        return (uint64)regs;
    case sysPipeAttach:
        regs->rax = (uint64)vfsPipeAttach((int)regs->rdi, regs->rsi != 0);
        return (uint64)regs;
    case sysKill: {
        /* Real kill(2)'s own pid<0/pid==0 convention (userKill's `pid` is
           already a signed `long`, see ulib.h -- only this kernel-side
           interpretation of the raw bit pattern was missing): pid>0 is the
           single-target path this always had; pid==0 targets the caller's
           OWN process group (threadKillGroup, sched.c -- naturally kills
           no one else if this is the group's only OTHER member, since
           threadKill itself already excludes the caller's own Running
           thread); pid<0 targets group -pid, exactly like real
           kill(-pgid, SIGKILL). */
        int64 rawPid = (int64)regs->rdi;
        bool ok;
        if (rawPid > 0)
            ok = threadKill((uint32)rawPid);
        else
            ok = threadKillGroup(rawPid == 0 ? currentThread()->pgid : (uint32)(-rawPid));
        regs->rax = ok ? 0 : (uint64)-1;
        return (uint64)regs;
    }
    case sysSigSend: {
        /* Same pid>0/pid==0/pid<0 convention as sysKill just above, this
           time through sigRaiseGroup (signal.h) for the two group cases --
           see that function's own doc comment for why every per-target
           special case (SIGKILL/SIGSTOP/SIGCONT bypasses, early-wake,
           SigInfo) still applies exactly as it already does for a plain,
           single-target send. */
        int64 rawPid = (int64)regs->rdi;
        int signum = (int)regs->rsi;
        uint32 senderPid = currentThreadId();
        bool isGroup = (rawPid <= 0);
        uint32 targetPgid = isGroup ? (rawPid == 0 ? currentThread()->pgid : (uint32)(-rawPid)) : 0;

        /* siCodeUser/senderPid: a real userSigSend always has a genuine
           sending process, unlike sched.c's own kernel-internal raises
           (SIGALRM/SIGCHLD) or keyboard.c's terminal-generated ones --
           exactly real Unix's own SI_USER, si_pid the caller's own pid,
           exactly like real kill(2). */
        bool ok = isGroup
            ? sigRaiseGroup(targetPgid, signum, siCodeUser, (int)senderPid)
            : sigRaiseInfo((uint32)rawPid, signum, siCodeUser, (int)senderPid, 0);
        regs->rax = ok ? 0 : (uint64)-1;

        /* Every OTHER outcome of sigRaise/sigRaiseGroup either changes
           nothing about the CALLING thread's own state (a signal aimed at
           someone else) or, for an ordinary catchable signal aimed at
           self, is already caught by isrDispatch's unconditional
           deliverPendingSignals call right after this handler returns
           (see isr.c) -- self-signaling sigUsr1 and having its handler
           fire before ever reaching userspace again already works today
           without any special case here. SIGSTOP against the caller's own
           pid (or a group that includes it) is the one exception:
           threadStop (called from sigRaise above, once per group member
           for the group case) already flipped this very thread's state
           away from Running, but sigRaise/sysSigSend never touch
           sigPending for it (see signal.h), so deliverPendingSignals'
           pending-bit scan would find nothing and simply hand `regs`
           straight back -- returning to ring 3 and continuing to run a
           thread the scheduler no longer considers Runnable, until the
           next timer tick eventually notices. Explicit reschedule here
           closes that gap, the same "flip state, then let schedule() do
           the actual waiting" pattern sysSleep/sysExit already use for
           their own self-directed state changes. The group case checks
           targetPgid against the caller's OWN pgid rather than an exact id
           match, since the caller itself -- not just some other member --
           is who this reschedule is actually about. */
        bool selfTargeted = isGroup ? (targetPgid == currentThread()->pgid) : ((uint32)rawPid == senderPid);
        if (ok && signum == sigStop && selfTargeted)
            return schedule((uint64)regs);
        return (uint64)regs;
    }
    case sysSigAction:
        regs->rax = sigSetHandler(currentThread(), (int)regs->rdi, regs->rsi, regs->rdx, regs->r10);
        return (uint64)regs;
    case sysSigProcMask:
        regs->rax = sigProcMask(currentThread(), (int)regs->rdi, regs->rsi);
        return (uint64)regs;
    case sysSigReturn:
        sigOnSigReturn(currentThread(), regs);
        return (uint64)regs;
    case sysAlarm:
        regs->rax = threadSetAlarm(regs->rdi);
        return (uint64)regs;
    case sysSetpgid: {
        /* Real setpgid(2)'s own "0 means self"/"0 means use pid itself as
           the new group id" resolution -- threadSetPgid (sched.h) takes an
           already-fully-resolved value, exactly like threadSetBrk/
           threadSetAlarm above do for THEIR raw syscall arguments. */
        uint32 pid = (regs->rdi == 0) ? currentThreadId() : (uint32)regs->rdi;
        uint32 pgid = (regs->rsi == 0) ? pid : (uint32)regs->rsi;
        regs->rax = threadSetPgid(pid, pgid) ? 0 : (uint64)-1;
        return (uint64)regs;
    }
    case sysGetpgid: {
        uint32 pid = (regs->rdi == 0) ? currentThreadId() : (uint32)regs->rdi;
        uint32 pgid = threadPgid(pid);
        regs->rax = (pgid == threadInvalidId) ? (uint64)-1 : (uint64)pgid;
        return (uint64)regs;
    }
    case sysNice: {
        /* Real setpriority(PRIO_PROCESS, ...)'s own "0 means the caller
           itself" convention, same resolution shape as sysSetpgid's `pid`
           argument just above -- rdi/pid, rsi/weight, straight through to
           threadSetPriority (sched.h), which now does the SAME id==0/
           out-of-range/Dead-or-Unused rejection threadSetPgid already did
           (see that function's own updated doc comment for why it had to
           catch up once this syscall made it reachable from ring 3). */
        uint32 pid = (regs->rdi == 0) ? currentThreadId() : (uint32)regs->rdi;
        uint32 weight = (uint32)regs->rsi;
        regs->rax = threadSetPriority(pid, weight) ? 0 : (uint64)-1;
        return (uint64)regs;
    }
    case sysGetPriority: {
        uint32 pid = (regs->rdi == 0) ? currentThreadId() : (uint32)regs->rdi;
        uint32 weight = threadPriority(pid);
        regs->rax = (weight == 0) ? (uint64)-1 : (uint64)weight;
        return (uint64)regs;
    }
    case sysTime:
        /* Real time(2)'s job: seconds since the Unix epoch, off the real
           CMOS RTC (rtc.c) -- the first syscall in this kernel answering
           "what day is it" instead of "how long since boot" (sysSleep,
           userAlarm). No arguments, nothing to validate. */
        regs->rax = (uint64)rtcNow();
        return (uint64)regs;
    case sysSetFgPgrp:
        /* Real tcsetpgrp(3)'s job -- names which process group keyboard.c's
           real Ctrl-C/Ctrl-Z (SIGINT/SIGTSTP) is scoped to next. No
           permission check at all (this kernel has no notion of "the
           controlling terminal of MY session" to check ownership against,
           the same honest, documented omission threadSetPgid's own doc
           comment already gives for setpgid's session check) -- any
           process can claim the foreground for any pgid, including one it
           is not even a member of. */
        keyboardSetForegroundPgid((uint32)regs->rdi);
        regs->rax = 0;
        return (uint64)regs;
    case sysGetFgPgrp:
        regs->rax = (uint64)keyboardForegroundPgid();
        return (uint64)regs;
    case sysPoll: {
        /* Real poll(2): rdi/PollFd array, rsi/nfds, rdx/timeoutMs (signed,
           -1 = wait forever, 0 = report current readiness without ever
           blocking, like every real poll(2) implementation). Flat address
           space (this kernel's honest fork()-less design, see doSpawn's own
           comment) means the array can be read and written in place, the
           same way sysStat writes straight into its caller's FbStat*
           without a separate copy-out step. */
        uint64 nfds = regs->rsi;
        int64 timeoutMs = (int64)regs->rdx;
        if (nfds > maxPollFds || !userRangeOk(regs->rdi, nfds * sizeof(PollFd))) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        PollFd* userFds = (PollFd*)regs->rdi;
        Thread* self = currentThread();

        /* First entry into this call (not a retry re-executing after a
           park below) picks the deadline once and remembers it in
           pollDeadline -- a later retry must NOT recompute it from the
           still-unchanged rdx, or a poll that keeps almost-but-not-quite
           finding a ready fd could have its effective timeout pushed out
           indefinitely instead of counting down real elapsed time. */
        if (!self->pollActive && timeoutMs != 0) {
            self->pollActive = true;
            if (timeoutMs < 0) {
                self->pollDeadline = 0;
            } else {
                uint64 ticks = ((uint64)timeoutMs * (uint64)pitFrequency()) / 1000;
                if (ticks == 0)
                    ticks = 1;
                self->pollDeadline = pitTicks() + ticks;
            }
        }

        int64 ready = 0;
        for (uint64 i = 0; i < nfds; i++) {
            int16 revents = 0;
            if (vfsPollReady(userFds[i].fd, userFds[i].events, &revents))
                ready++;
            userFds[i].revents = revents;
        }

        bool expired = self->pollActive && self->pollDeadline != 0 &&
                       pitTicks() >= self->pollDeadline;
        if (ready > 0 || timeoutMs == 0 || expired) {
            self->pollActive = false;
            self->pollDeadline = 0;
            regs->rax = (uint64)ready;
            return (uint64)regs;
        }

        /* None ready yet, timeout not yet reached: park on the one shared
           channel every poller blocks on (schedPollChannel, sched.h) and
           rewind rip so the whole syscall re-executes -- the same
           restart-the-syscall trick sysRead/sysWrite/sysWait already use,
           except schedule()'s own expiry scan (sched.c) is what guarantees
           this retry eventually happens even if no fd ever becomes ready. */
        self->sigInRetryableSyscall = true;
        schedBlockCurrent(schedPollChannel());
        regs->rip -= 2;
        return schedule((uint64)regs);
    }
    case sysMkfifo: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsMkfifo(path);
        return (uint64)regs;
    }
    case sysExec: {
        char path[256];
        char argvStorage[elfMaxArgs][execArgMaxLen];
        const char* argvPtrs[elfMaxArgs];
        int argc;
        if (!copyUserPath(regs->rdi, path, sizeof(path)) ||
            !copyUserArgv(regs->rsi, argvStorage, argvPtrs, &argc) ||
            !doExec(path, argvPtrs, argc, regs))
            regs->rax = (uint64)-1;
        return (uint64)regs;
    }
    case sysSpawn: {
        char path[256];
        char argvStorage[elfMaxArgs][execArgMaxLen];
        const char* argvPtrs[elfMaxArgs];
        int argc;
        if (!copyUserPath(regs->rdi, path, sizeof(path)) ||
            !copyUserArgv(regs->rsi, argvStorage, argvPtrs, &argc))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)doSpawn(path, argvPtrs, argc);
        return (uint64)regs;
    }
    case sysWait: {
        uint32 id = (uint32)regs->rdi;
        if (id == waitAnyPid) {
            /* waitpid(-1)-style "wait for ANY child" -- see
               threadJoinAnyChild's own doc comment (sched.h) for the full
               contract. regs->rsi is an optional user `long*` pointer
               (userWaitAny's outPid, ulib.c) that receives which pid
               actually got reaped; nullptr means the caller does not care.
               Written as a full 8-byte store below to match `long`'s real
               width, not truncated to sizeof(uint32) -- a narrower store
               would leave the user variable's upper 4 bytes as whatever
               uninitialized garbage the caller's stack already had. */
            if (regs->rsi != 0 && !userRangeOk(regs->rsi, sizeof(uint64))) {
                regs->rax = (uint64)-1;
                return (uint64)regs;
            }
            uint32 reapedId;
            int64 code;
            WaitScanResult scan = threadJoinAnyChild(currentThreadId(), &reapedId, &code);
            if (scan == waitScanNoChildren) {
                regs->rax = (uint64)-1;
                return (uint64)regs;
            }
            if (scan == waitScanNoneDead) {
                /* Same retry-syscall dance as the specific-pid path below,
                   just parked on our OWN slot instead of a named child's --
                   threadExit/threadKill wake this channel for EVERY dying
                   child of ours, not just one in particular (see
                   threadJoinAnyChild's doc comment), so re-scanning from
                   scratch on each retry is what actually picks up whichever
                   one died. */
                currentThread()->sigInRetryableSyscall = true;
                schedBlockCurrent(currentThread());
                regs->rip -= 2;
                return schedule((uint64)regs);
            }
            if (regs->rsi != 0)
                *(uint64*)regs->rsi = (uint64)reapedId;
            regs->rax = (uint64)code;
            return (uint64)regs;
        }
        if (id == 0 || id >= threadCapacity()) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        Thread* target = threadAt(id);
        if (target->state == threadUnused) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        if (target->state != threadDead) {
            /* Not dead yet: parking here and calling halt() the way
               threadJoin (sched.c) does for a plain kernel thread body
               would hang the whole machine, not just this thread -- this
               handler runs with interrupts disabled (int 0x80 is an
               interrupt gate, idt.c) on the single shared bootstrap kernel
               stack (isrStubs.S/tss.c), not the calling thread's own
               persistent one, so unlike kmain.c's joinerThread it cannot
               literally halt in place and pick its own C call frame back
               up later: hlt with IF=0 never wakes, since nothing can ever
               deliver the timer interrupt that would run schedule() again.
               Instead: block on the same channel threadExit wakes
               (schedWakeChannel(&threads[id])), rewind rip by the 2 bytes
               of the `int $0x80` opcode this trap just came from so it
               re-executes once this thread is scheduled again, and hand
               the CPU to schedule() -- the same "flip state, then let the
               interrupt-return path do the actual waiting" technique
               sysSleep already uses. The retry re-enters this exact case
               with the same rax/rdi; since threadExit will have already
               flipped the target to Dead before waking this thread back
               up, it takes the branch below next time and completes
               without blocking again. */
            currentThread()->sigInRetryableSyscall = true;
            schedBlockCurrent(target);
            regs->rip -= 2;
            return schedule((uint64)regs);
        }
        regs->rax = (uint64)threadJoin(id);
        return (uint64)regs;
    }
    case sysMmap: {
        /* addrHint==0 means "kernel picks" (mmapReserve's own first-fit,
           real mmap(NULL, ...) shape); a nonzero addrHint is taken as an
           exact placement request, refused (not silently relocated) if it
           does not fit -- there is no MAP_FIXED bit to distinguish the two,
           every addrHint here behaves like real mmap's own weaker "hint"
           contract already narrowed down to "exact or refused". fd/offset
           are read (and validated below) even when MAP_ANONYMOUS is set,
           harmlessly ignored by mmapReserve itself in that case, exactly
           like real mmap(2)'s own documented behavior. */
        uint64 addrHint = regs->rdi;
        uint64 length = regs->rsi;
        uint32 prot = (uint32)regs->rdx;
        uint32 flags = (uint32)regs->r10;
        int fd = (int)regs->r8;
        uint64 offset = regs->r9;
        bool anonymous = (flags & mmapAnonymous) != 0;

        if (!anonymous) {
            /* Same three seekable, real-data fd kinds vfsLseek itself
               already restricts to (vfs.c) -- a pipe/fifo/console/pidfd fd
               has no fixed byte range a page fault could ever re-read
               deterministically, so mmap refuses them here rather than
               letting mmapFaultHandler discover that failure lazily, one
               page at a time, later. */
            if (fd < 0 || fd >= maxOpenFiles) {
                regs->rax = (uint64)-1;
                return (uint64)regs;
            }
            FileDescriptor* fdEntry = &currentThread()->fds[fd];
            if (fdEntry->kind != fdFile && fdEntry->kind != fdSfs && fdEntry->kind != fdBlockDev) {
                regs->rax = (uint64)-1;
                return (uint64)regs;
            }
        }

        uint64 mappedAddr;
        if (!mmapReserve(currentThread(), addrHint, length, prot, anonymous, fd, offset, &mappedAddr)) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        regs->rax = mappedAddr;
        return (uint64)regs;
    }
    case sysMunmap:
        regs->rax = mmapRelease(currentThread(), regs->rdi, regs->rsi) ? 0 : (uint64)-1;
        return (uint64)regs;
    default:
        regs->rax = (uint64)-1;
        return (uint64)regs;
    }
}
