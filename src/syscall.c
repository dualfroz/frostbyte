#include "syscall.h"
#include "isr.h"
#include "console.h"
#include "sched.h"
#include "vfs.h"
#include "fs.h"
#include "string.h"

#define userSpaceMin 0x0000000040000000ull
#define userSpaceMax 0x0000800000000000ull

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

uint64 syscallDispatch(Registers* regs)
{
    switch (regs->rax) {
    case sysWrite:
        regs->rax = (uint64)doWrite(regs->rdi, regs->rsi, regs->rdx);
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
