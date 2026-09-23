#ifndef FROSTBYTE_ULIB_H
#define FROSTBYTE_ULIB_H

#include "syscall.h"
#include "fs.h"
#include "signalnum.h"
#include "poll.h"

typedef void (*SigHandler)(int signum);

typedef void (*SigInfoHandler)(int signum, SigInfo* info, void* context);

long userWrite(int fd, const char* buffer, unsigned long length);
long userRead(int fd, char* buffer, unsigned long length);
void userExit(int code);
long userYield(void);
long userGetpid(void);
void* userSbrk(long increment);
void userSleep(unsigned long milliseconds);

unsigned long userAlarm(unsigned long seconds);

long userPoll(PollFd* fds, unsigned long nfds, long timeoutMs);

long userOpen(const char* path, unsigned int flags);
long userClose(int fd);
long userLseek(int fd, long offset, int whence);
long userStat(const char* path, FbStat* out);
long userGetdents(int fd, FbDirent* out, unsigned long length);
long userMkdir(const char* path);
long userUnlink(const char* path);

long userRmdir(const char* path);

long userChmod(const char* path, unsigned int mode);

long userChown(const char* path, unsigned int uid);

long userGetuid(void);
long userSetuid(unsigned int uid);

long userGetgid(void);
long userSetgid(unsigned int gid);

long userChgrp(const char* path, unsigned int gid);

long userChdir(const char* path);
long userGetcwd(char* buffer, unsigned long cap);

long userGetEnv(const char* name, char* buffer, unsigned long cap);
long userSetEnv(const char* name, const char* value);

long userUnsetEnv(const char* name);

long userPipe(int fds[2]);

long userPipeCreate(void);
long userPipeAttach(long handle, int wantWrite);
long userSymlink(const char* target, const char* path);
long userReadlink(const char* path, char* buffer, unsigned long cap);
long userLink(const char* existingPath, const char* newPath);

long userMkfifo(const char* path);
long userExec(const char* path, char* const argv[]);
long userSpawn(const char* path, char* const argv[]);
long userWait(long pid);

long userWaitAny(long* outPid);

long userKill(long pid);

long userSigSend(long pid, int signum);

long userPidfdOpen(long pid);

long userPidfdSend(long fd, int signum);

long userPidfdWait(long fd);

void* userMmap(void* addrHint, unsigned long length, int prot, int flags, int fd, long offset);

#define userMmapFailed ((void*)-1)

long userMunmap(void* addr, unsigned long length);

long userSigAction(int signum, SigHandler handler, unsigned long mask, unsigned long flags);

long userSigActionInfo(int signum, SigInfoHandler handler, unsigned long mask, unsigned long flags);

unsigned long userSigProcMask(int how, unsigned long mask);

long userSetpgid(long pid, long pgid);

long userGetpgid(long pid);

long userNice(long pid, long weight);

long userGetPriority(long pid);

long userTime(void);

long userSetForegroundPgid(long pgid);

long userGetForegroundPgid(void);

void print(const char* text);
void printParts(const char* const* parts);
void printDec(long value);
unsigned long stringLength(const char* text);

void decToString(long value, char* out);

long parseDec(const char* text);

#endif
