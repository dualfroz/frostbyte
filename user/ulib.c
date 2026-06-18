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
