#ifndef FROSTBYTE_SCHED_H
#define FROSTBYTE_SCHED_H

#include "types.h"
#include "vfs.h"
#include "prioq.h"
#include "isr.h"
#include "signalnum.h"
#include "mmap.h"

#define schedDefaultWeight 1024u

#define threadInvalidId 0xFFFFFFFFu

#define threadNameMax 32

#define envMaxVars 8
#define envEntryMax 64

#define sigMaxSignals 32

#define sigMaxNestDepth 4

typedef enum {
    threadUnused = 0,
    threadRunnable = 1,
    threadRunning = 2,
    threadSleeping = 3,
    threadDead = 4,
    threadBlocked = 5,
    threadStopped = 6
} ThreadState;

typedef void (*ThreadEntry)(void);

typedef struct Thread {
    uint64 rsp;
    uint64 stackBase;
    uint64 kernelStackTop;
    uint64 sleepUntil;
    uint64 alarmAt;
    bool pollActive;
    uint64 pollDeadline;
    uint64 brk;
    uint64 vruntime;
    uint64 exitCode;
    uint32 weight;
    uint32 id;
    uint32 parentId;
    uint32 generation;
    uint32 pgid;
    uint32 uid;
    uint32 gid;
    char cwd[fsPathMax];
    char env[envMaxVars][envEntryMax];
    int envCount;
    ThreadState state;
    bool everRan;
    char name[threadNameMax];
    const void* waitChannel;
    uint64 userSlotBase;
    PrioQNode schedNode;
    FileDescriptor fds[maxOpenFiles];
    Vma vmas[maxVmasPerThread];
    uint64 sigPending;
    uint64 sigBlocked;
    uint64 sigHandlers[sigMaxSignals];
    uint64 sigHandlerMasks[sigMaxSignals];
    uint64 sigHandlerFlags[sigMaxSignals];
    SigInfo sigPendingInfo[sigMaxSignals];
    bool sigInRetryableSyscall;
    uint32 sigHandlerDepth;
    uint64 sigSavedBlocked[sigMaxNestDepth];
    Registers sigSavedRegs[sigMaxNestDepth];
} Thread;

void schedInit(void);

uint32 threadCreate(const char* name, ThreadEntry entry);
uint32 threadCreateUser(const char* name, uint64 entry, uint64 userStackTop);
uint64 schedule(uint64 currentRsp);
void schedStart(void);

uint32 threadCapacity(void);

bool threadReap(uint32 id);

bool threadDiscard(uint32 id);

int64 threadJoin(uint32 id);

typedef enum { waitScanNoChildren, waitScanNoneDead, waitScanFoundDead } WaitScanResult;
WaitScanResult threadJoinAnyChild(uint32 parentId, uint32* outId, int64* outCode);

void threadExit(uint64 code);

#define threadKilledExitCode 9u

bool threadKill(uint32 id);

bool threadStop(uint32 id);

bool threadContinue(uint32 id);

bool threadWakeForSignal(uint32 id);

void threadSleep(uint64 milliseconds);
void threadSetBrk(uint32 id, uint64 brk);
uint64 currentThreadSbrk(int64 increment);

uint64 threadSetAlarm(uint64 seconds);

bool threadSetPriority(uint32 id, uint32 weight);

uint32 threadPriority(uint32 id);
uint64 threadVruntime(uint32 id);

uint32 threadPgid(uint32 id);

bool threadSetPgid(uint32 id, uint32 pgid);

bool threadKillGroup(uint32 pgid);

const char* currentThreadCwd(void);
bool currentThreadSetCwd(const char* path);

uint32 currentThreadUid(void);
bool currentThreadSetUid(uint32 newUid);

uint32 currentThreadGid(void);
bool currentThreadSetGid(uint32 newGid);

const char* currentThreadGetEnv(const char* name);
bool currentThreadSetEnv(const char* name, const char* value);

bool currentThreadUnsetEnv(const char* name);

int currentThreadEnvCount(void);
const char* currentThreadEnvEntry(int index);

void threadSetName(uint32 id, const char* name);

uint64 threadUserSlotBase(uint32 id);

void threadSetUserSlotBase(uint32 id, uint64 base);

bool threadUserSlotInUse(uint64 slotBase);

bool threadUserSlotHeldByOther(uint64 slotBase, uint32 exceptId);

bool threadImageSlotBusy(const void* image, uint64 imageSize, uint32 exceptId);

void schedBlockCurrent(const void* channel);
void schedCancelBlockCurrent(void);
void schedWakeChannel(const void* channel);

const void* schedPollChannel(void);

uint32 threadCount(void);
Thread* threadAt(uint32 index);
Thread* currentThread(void);
uint32 currentThreadId(void);

#endif
