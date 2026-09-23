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

static int64 loadImageByPath(const char* path, uint32 slotOwner, LoadedImage* out)
{
    VfsNode* node = vfsLookup(path);
    if (!node)
        return -1;
    if (!vfsPermitted(vfsMode(node), vfsUid(node), vfsGid(node), currentThreadUid(), currentThreadGid(), fsModeOwnerExec))
        return -1;

    const void* image = vfsFileData(node);
    uint64 size = vfsSize(node);
    if (threadImageSlotBusy(image, size, slotOwner))
        return spawnSlotBusy;
    uint64 entry = elfLoad(image, size);
    if (entry == 0)
        return -1;

    out->entry = entry;
    out->stackTop = elfSlotStackTop(entry);
    out->brkStart = elfSlotBrkStart(entry);

    return pagingMapUserRangeFlags(out->stackTop - execStackSize, execStackSize, pageNoExecute) ? 0 : -1;
}

static int64 doExec(const char* path, const char* const* argv, int argc, Registers* regs)
{
    uint32 pid = currentThreadId();
    uint64 oldSlotBase = threadUserSlotBase(pid);

    LoadedImage img;
    int64 loadStatus = loadImageByPath(path, pid, &img);
    if (loadStatus != 0)
        return loadStatus;

    uint64 initialRsp;
    if (!elfBuildInitialStack(img.stackTop, execStackSize, argv, argc, &initialRsp))
        return -1;

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
    return 0;
}

static int64 doSpawn(const char* path, const char* const* argv, int argc)
{
    LoadedImage img;
    int64 loadStatus = loadImageByPath(path, threadInvalidId, &img);
    if (loadStatus != 0)
        return loadStatus;

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
    currentThread()->sigInRetryableSyscall = false;

    switch (regs->rax) {
    case sysWrite: {
        int64 result = doWrite(regs->rdi, regs->rsi, regs->rdx);
        if (result == -1) {
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
        if (!copyUserPath(regs->rdi, path, sizeof(path))) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        const void* channel = vfsFifoWriterWaitChannel(path, (uint32)regs->rsi);
        if (channel) {
            currentThread()->sigInRetryableSyscall = true;
            schedBlockCurrent(channel);
            regs->rip -= 2;
            return schedule((uint64)regs);
        }
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
        regs->rax = (uint64)currentThreadUid();
        return (uint64)regs;
    case sysSetuid:
        regs->rax = currentThreadSetUid((uint32)regs->rdi) ? 0 : (uint64)-1;
        return (uint64)regs;
    case sysGetgid:
        regs->rax = (uint64)currentThreadGid();
        return (uint64)regs;
    case sysSetgid:
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
        regs->rax = (uint64)vfsPidfdOpen((uint32)regs->rdi);
        return (uint64)regs;
    case sysPidfdSend: {
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
        int64 rawPid = (int64)regs->rdi;
        int signum = (int)regs->rsi;
        uint32 senderPid = currentThreadId();
        bool isGroup = (rawPid <= 0);
        uint32 targetPgid = isGroup ? (rawPid == 0 ? currentThread()->pgid : (uint32)(-rawPid)) : 0;

        bool ok = isGroup
            ? sigRaiseGroup(targetPgid, signum, siCodeUser, (int)senderPid)
            : sigRaiseInfo((uint32)rawPid, signum, siCodeUser, (int)senderPid, 0);
        regs->rax = ok ? 0 : (uint64)-1;

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
        regs->rax = (uint64)rtcNow();
        return (uint64)regs;
    case sysSetFgPgrp:
        keyboardSetForegroundPgid((uint32)regs->rdi);
        regs->rax = 0;
        return (uint64)regs;
    case sysGetFgPgrp:
        regs->rax = (uint64)keyboardForegroundPgid();
        return (uint64)regs;
    case sysPoll: {
        uint64 nfds = regs->rsi;
        int64 timeoutMs = (int64)regs->rdx;
        if (nfds > maxPollFds || !userRangeOk(regs->rdi, nfds * sizeof(PollFd))) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        PollFd* userFds = (PollFd*)regs->rdi;
        Thread* self = currentThread();

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
            !copyUserArgv(regs->rsi, argvStorage, argvPtrs, &argc)) {
            regs->rax = (uint64)-1;
            return (uint64)regs;
        }
        int64 execStatus = doExec(path, argvPtrs, argc, regs);
        if (execStatus != 0)
            regs->rax = (uint64)execStatus;
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
            currentThread()->sigInRetryableSyscall = true;
            schedBlockCurrent(target);
            regs->rip -= 2;
            return schedule((uint64)regs);
        }
        regs->rax = (uint64)threadJoin(id);
        return (uint64)regs;
    }
    case sysMmap: {
        uint64 addrHint = regs->rdi;
        uint64 length = regs->rsi;
        uint32 prot = (uint32)regs->rdx;
        uint32 flags = (uint32)regs->r10;
        int fd = (int)regs->r8;
        uint64 offset = regs->r9;
        bool anonymous = (flags & mmapAnonymous) != 0;

        if (!anonymous) {
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
