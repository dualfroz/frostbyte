#ifndef FROSTBYTE_ULIB_H
#define FROSTBYTE_ULIB_H

#include "syscall.h"
#include "fs.h"

long userWrite(int fd, const char* buffer, unsigned long length);
long userRead(int fd, char* buffer, unsigned long length);
void userExit(int code);
long userYield(void);
long userGetpid(void);
void* userSbrk(long increment);
void userSleep(unsigned long milliseconds);

long userOpen(const char* path, unsigned int flags);
long userClose(int fd);
long userLseek(int fd, long offset, int whence);
long userStat(const char* path, FbStat* out);
long userGetdents(int fd, FbDirent* out, unsigned long length);
long userMkdir(const char* path);
long userUnlink(const char* path);

void print(const char* text);
void printDec(long value);
unsigned long stringLength(const char* text);

#endif
