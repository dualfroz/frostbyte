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
    case sysRead:
        regs->rax = (uint64)doRead(regs->rdi, regs->rsi, regs->rdx);
        return (uint64)regs;
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
    case sysUnlink: {
        char path[256];
        if (!copyUserPath(regs->rdi, path, sizeof(path)))
            regs->rax = (uint64)-1;
        else
            regs->rax = (uint64)(int64)vfsUnlink(path);
        return (uint64)regs;
    }
    default:
        regs->rax = (uint64)-1;
        return (uint64)regs;
    }
}
