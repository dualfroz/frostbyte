#ifndef FROSTBYTE_SIGNAL_H
#define FROSTBYTE_SIGNAL_H

#include "types.h"
#include "isr.h"
#include "sched.h"
#include "signalnum.h"

#define sigAllBlocked (~0ull)

#define sigTrampolineVirtualAddr 0x0000700000000000ull

#define sigInfoVirtualAddr 0x0000700000010000ull

#define sigInfoMaxPages 8

typedef enum {
    sigOutcomeNone = 0,
    sigOutcomeHandler = 1,
    sigOutcomeTerminate = 2,
    sigOutcomeStop = 3
} SigOutcome;

void sigResetState(Thread* thread);

bool sigRaise(uint32 targetId, int signum);

bool sigRaiseInfo(uint32 targetId, int signum, int siCode, int siPid, int siStatus);

bool sigRaiseGroup(uint32 pgid, int signum, int siCode, int siPid);

uint64 sigSetHandler(Thread* thread, int signum, uint64 handlerAddr, uint64 mask, uint64 flags);

uint64 sigProcMask(Thread* thread, int how, uint64 mask);

SigOutcome sigConsumePending(Thread* thread, Registers* regs, int* outSignum);

bool sigOnSigReturn(Thread* thread, Registers* regs);

void sigInstallTrampoline(void);

void sigInstallInfoPage(void);

#endif
