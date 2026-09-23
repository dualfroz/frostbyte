#include "ulib.h"

static long syscall3(long number, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(number), "D"(a1), "S"(a2), "d"(a3)
        : "memory", "rcx", "r11");
    return ret;
}

static long syscall4(long number, long a1, long a2, long a3, long a4)
{
    long ret;
    register long a4Reg __asm__("r10") = a4;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(number), "D"(a1), "S"(a2), "d"(a3), "r"(a4Reg)
        : "memory", "rcx", "r11");
    return ret;
}

static long syscall6(long number, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long ret;
    register long a4Reg __asm__("r10") = a4;
    register long a5Reg __asm__("r8") = a5;
    register long a6Reg __asm__("r9") = a6;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(number), "D"(a1), "S"(a2), "d"(a3), "r"(a4Reg), "r"(a5Reg), "r"(a6Reg)
        : "memory", "rcx", "r11");
    return ret;
}

long userWrite(int fd, const char* buffer, unsigned long length)
{
    return syscall3(sysWrite, fd, (long)buffer, (long)length);
}

long userRead(int fd, char* buffer, unsigned long length)
{
    return syscall3(sysRead, fd, (long)buffer, (long)length);
}

void userExit(int code)
{
    syscall3(sysExit, code, 0, 0);
}

long userYield(void)
{
    return syscall3(sysYield, 0, 0, 0);
}

long userGetpid(void)
{
    return syscall3(sysGetpid, 0, 0, 0);
}

void* userSbrk(long increment)
{
    return (void*)syscall3(sysSbrk, increment, 0, 0);
}

void userSleep(unsigned long milliseconds)
{
    syscall3(sysSleep, (long)milliseconds, 0, 0);
}

unsigned long userAlarm(unsigned long seconds)
{
    return (unsigned long)syscall3(sysAlarm, (long)seconds, 0, 0);
}

long userPoll(PollFd* fds, unsigned long nfds, long timeoutMs)
{
    return syscall3(sysPoll, (long)fds, (long)nfds, timeoutMs);
}

long userOpen(const char* path, unsigned int flags)
{
    return syscall3(sysOpen, (long)path, (long)flags, 0);
}

long userClose(int fd)
{
    return syscall3(sysClose, fd, 0, 0);
}

long userLseek(int fd, long offset, int whence)
{
    return syscall3(sysLseek, fd, offset, whence);
}

long userStat(const char* path, FbStat* out)
{
    return syscall3(sysStat, (long)path, (long)out, 0);
}

long userGetdents(int fd, FbDirent* out, unsigned long length)
{
    return syscall3(sysGetdents, fd, (long)out, (long)length);
}

long userMkdir(const char* path)
{
    return syscall3(sysMkdir, (long)path, 0, 0);
}

long userUnlink(const char* path)
{
    return syscall3(sysUnlink, (long)path, 0, 0);
}

long userRmdir(const char* path)
{
    return syscall3(sysRmdir, (long)path, 0, 0);
}

long userChmod(const char* path, unsigned int mode)
{
    return syscall3(sysChmod, (long)path, (long)mode, 0);
}

long userChown(const char* path, unsigned int uid)
{
    return syscall3(sysChown, (long)path, (long)uid, 0);
}

long userGetuid(void)
{
    return syscall3(sysGetuid, 0, 0, 0);
}

long userSetuid(unsigned int uid)
{
    return syscall3(sysSetuid, (long)uid, 0, 0);
}

long userGetgid(void)
{
    return syscall3(sysGetgid, 0, 0, 0);
}

long userSetgid(unsigned int gid)
{
    return syscall3(sysSetgid, (long)gid, 0, 0);
}

long userChgrp(const char* path, unsigned int gid)
{
    return syscall3(sysChgrp, (long)path, (long)gid, 0);
}

long userChdir(const char* path)
{
    return syscall3(sysChdir, (long)path, 0, 0);
}

long userGetcwd(char* buffer, unsigned long cap)
{
    return syscall3(sysGetcwd, (long)buffer, (long)cap, 0);
}

long userGetEnv(const char* name, char* buffer, unsigned long cap)
{
    return syscall3(sysGetEnv, (long)name, (long)buffer, (long)cap);
}

long userSetEnv(const char* name, const char* value)
{
    return syscall3(sysSetEnv, (long)name, (long)value, 0);
}

long userUnsetEnv(const char* name)
{
    return syscall3(sysUnsetEnv, (long)name, 0, 0);
}

long userPipe(int fds[2])
{
    return syscall3(sysPipe, (long)fds, 0, 0);
}

long userPipeCreate(void)
{
    return syscall3(sysPipeCreate, 0, 0, 0);
}

long userPipeAttach(long handle, int wantWrite)
{
    return syscall3(sysPipeAttach, handle, wantWrite, 0);
}

long userSymlink(const char* target, const char* path)
{
    return syscall3(sysSymlink, (long)target, (long)path, 0);
}

long userReadlink(const char* path, char* buffer, unsigned long cap)
{
    return syscall3(sysReadlink, (long)path, (long)buffer, (long)cap);
}

long userLink(const char* existingPath, const char* newPath)
{
    return syscall3(sysLink, (long)existingPath, (long)newPath, 0);
}

long userMkfifo(const char* path)
{
    return syscall3(sysMkfifo, (long)path, 0, 0);
}

long userExec(const char* path, char* const argv[])
{
    return syscall3(sysExec, (long)path, (long)argv, 0);
}

long userSpawn(const char* path, char* const argv[])
{
    return syscall3(sysSpawn, (long)path, (long)argv, 0);
}

long userWait(long pid)
{
    return syscall3(sysWait, pid, 0, 0);
}

long userWaitAny(long* outPid)
{
    return syscall3(sysWait, (long)waitAnyPid, (long)outPid, 0);
}

long userKill(long pid)
{
    return syscall3(sysKill, pid, 0, 0);
}

long userSigSend(long pid, int signum)
{
    return syscall3(sysSigSend, pid, signum, 0);
}

long userPidfdOpen(long pid)
{
    return syscall3(sysPidfdOpen, pid, 0, 0);
}

long userPidfdSend(long fd, int signum)
{
    return syscall3(sysPidfdSend, fd, signum, 0);
}

long userPidfdWait(long fd)
{
    return syscall3(sysPidfdWait, fd, 0, 0);
}

void* userMmap(void* addrHint, unsigned long length, int prot, int flags, int fd, long offset)
{
    return (void*)syscall6(sysMmap, (long)addrHint, (long)length, prot, flags, fd, offset);
}

long userMunmap(void* addr, unsigned long length)
{
    return syscall3(sysMunmap, (long)addr, (long)length, 0);
}

long userSigAction(int signum, SigHandler handler, unsigned long mask, unsigned long flags)
{
    return syscall4(sysSigAction, signum, (long)handler, (long)mask, (long)flags);
}

long userSigActionInfo(int signum, SigInfoHandler handler, unsigned long mask, unsigned long flags)
{
    return syscall4(sysSigAction, signum, (long)handler, (long)mask, (long)flags);
}

unsigned long userSigProcMask(int how, unsigned long mask)
{
    return (unsigned long)syscall3(sysSigProcMask, how, (long)mask, 0);
}

long userSetpgid(long pid, long pgid)
{
    return syscall3(sysSetpgid, pid, pgid, 0);
}

long userGetpgid(long pid)
{
    return syscall3(sysGetpgid, pid, 0, 0);
}

long userNice(long pid, long weight)
{
    return syscall3(sysNice, pid, weight, 0);
}

long userGetPriority(long pid)
{
    return syscall3(sysGetPriority, pid, 0, 0);
}

long userTime(void)
{
    return syscall3(sysTime, 0, 0, 0);
}

long userSetForegroundPgid(long pgid)
{
    return syscall3(sysSetFgPgrp, pgid, 0, 0);
}

long userGetForegroundPgid(void)
{
    return syscall3(sysGetFgPgrp, 0, 0, 0);
}

unsigned long stringLength(const char* text)
{
    unsigned long length = 0;
    while (text[length])
        length++;
    return length;
}

void print(const char* text)
{
    userWrite(1, text, stringLength(text));
}

void printParts(const char* const* parts)
{
    char line[256];
    unsigned long length = 0;
    for (int i = 0; parts[i]; i++)
        for (const char* c = parts[i]; *c && length < sizeof(line); c++)
            line[length++] = *c;
    userWrite(1, line, length);
}

void printDec(long value)
{
    unsigned long magnitude;
    if (value < 0) {
        userWrite(1, "-", 1);
        magnitude = (unsigned long)(-value);
    } else {
        magnitude = (unsigned long)value;
    }

    if (magnitude == 0) {
        userWrite(1, "0", 1);
        return;
    }

    char reversed[24];
    int length = 0;
    while (magnitude > 0) {
        reversed[length++] = (char)('0' + (magnitude % 10));
        magnitude /= 10;
    }

    char ordered[24];
    for (int i = 0; i < length; i++)
        ordered[i] = reversed[length - 1 - i];
    userWrite(1, ordered, (unsigned long)length);
}

void decToString(long value, char* out)
{
    unsigned long magnitude;
    int i = 0;
    if (value < 0) {
        out[i++] = '-';
        magnitude = (unsigned long)(-value);
    } else {
        magnitude = (unsigned long)value;
    }

    char digits[24];
    int length = 0;
    do {
        digits[length++] = (char)('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude > 0);

    while (length > 0)
        out[i++] = digits[--length];
    out[i] = 0;
}

long parseDec(const char* text)
{
    int i = 0;
    int negative = 0;
    if (text[0] == '-') {
        negative = 1;
        i = 1;
    }

    long value = 0;
    while (text[i] >= '0' && text[i] <= '9') {
        value = value * 10 + (text[i] - '0');
        i++;
    }
    return negative ? -value : value;
}
