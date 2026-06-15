#ifndef FROSTBYTE_SCHED_H
#define FROSTBYTE_SCHED_H

#include "types.h"
#include "vfs.h"

typedef enum {
    threadUnused = 0,
    threadRunnable = 1,
    threadRunning = 2,
    threadSleeping = 3,
    threadDead = 4
} ThreadState;

typedef void (*ThreadEntry)(void);

typedef struct Thread {
    uint64 rsp;
    uint64 stackBase;
    uint64 kernelStackTop;
    uint64 sleepUntil;
    uint64 brk;
    uint32 id;
    ThreadState state;
    const char* name;
    FileDescriptor fds[maxOpenFiles];
} Thread;

void schedInit(void);
uint32 threadCreate(const char* name, ThreadEntry entry);
uint32 threadCreateUser(const char* name, uint64 entry, uint64 userStackTop);
uint64 schedule(uint64 currentRsp);
void schedStart(void);

void threadExit(uint64 code);
void threadSleep(uint64 milliseconds);
void threadSetBrk(uint32 id, uint64 brk);
uint64 currentThreadSbrk(int64 increment);

uint32 threadCount(void);
Thread* threadAt(uint32 index);
Thread* currentThread(void);
uint32 currentThreadId(void);

#endif
