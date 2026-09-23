#include "signal.h"
#include "string.h"
#include "pmm.h"
#include "paging.h"
#include "ports.h"

extern char sigTrampolineStart[];
extern char sigTrampolineEnd[];

static uint64 sigInfoFrames[sigInfoMaxPages];
static uint32 sigInfoPageCount = 0;

#define sigInfoSlotsPerPage (frameSize / sizeof(SigInfo))

void sigResetState(Thread* thread)
{
    thread->sigPending = 0;
    thread->sigBlocked = 0;
    thread->sigHandlerDepth = 0;
    thread->sigInRetryableSyscall = false;
    memset(thread->sigHandlers, 0, sizeof(thread->sigHandlers));
    memset(thread->sigHandlerMasks, 0, sizeof(thread->sigHandlerMasks));
    memset(thread->sigHandlerFlags, 0, sizeof(thread->sigHandlerFlags));
    memset(thread->sigPendingInfo, 0, sizeof(thread->sigPendingInfo));
    memset(thread->sigSavedBlocked, 0, sizeof(thread->sigSavedBlocked));
    memset(thread->sigSavedRegs, 0, sizeof(thread->sigSavedRegs));
}

static bool sigDefaultIsIgnore(int signum)
{
    switch (signum) {
    case sigChld:
    case sigCont:
        return true;
    default:
        return false;
    }
}

static bool sigDefaultIsStop(int signum)
{
    return signum == sigTstp;
}

static bool sigWouldDeliver(const Thread* thread, int signum)
{
    uint64 bit = 1ull << (uint32)signum;
    if (thread->sigBlocked & bit)
        return false;

    uint64 handler = thread->sigHandlers[signum];
    if (handler == sigHandlerIgnore ||
        (handler == sigHandlerDefault && sigDefaultIsIgnore(signum)))
        return false;

    return true;
}

static bool raiseInfo(uint32 targetId, int signum, int siCode, int siPid, int siStatus)
{
    if (signum <= 0 || signum >= sigMaxSignals)
        return false;

    if (signum == sigKill)
        return threadKill(targetId);

    if (signum == sigStop)
        return threadStop(targetId);

    if (targetId == 0 || targetId >= threadCapacity())
        return false;

    Thread* target = threadAt(targetId);
    if (target->state == threadDead || target->state == threadUnused)
        return false;

    if (signum == sigCont)
        threadContinue(targetId);

    target->sigPending |= (1ull << (uint32)signum);

    target->sigPendingInfo[signum].siSignum = signum;
    target->sigPendingInfo[signum].siCode = siCode;
    target->sigPendingInfo[signum].siPid = siPid;
    target->sigPendingInfo[signum].siStatus = siStatus;

    if (sigWouldDeliver(target, signum))
        threadWakeForSignal(targetId);

    return true;
}

bool sigRaiseInfo(uint32 targetId, int signum, int siCode, int siPid, int siStatus)
{
    uint64 flags = saveAndDisableInterrupts();
    bool raised = raiseInfo(targetId, signum, siCode, siPid, siStatus);
    restoreInterrupts(flags);
    return raised;
}

bool sigRaise(uint32 targetId, int signum)
{
    return sigRaiseInfo(targetId, signum, siCodeKernel, 0, 0);
}

bool sigRaiseGroup(uint32 pgid, int signum, int siCode, int siPid)
{
    uint64 flags = saveAndDisableInterrupts();
    bool anyTarget = false;
    for (uint32 id = 1; id < threadCapacity(); id++) {
        Thread* candidate = threadAt(id);
        if (candidate->state == threadDead || candidate->state == threadUnused)
            continue;
        if (candidate->pgid != pgid)
            continue;

        anyTarget = true;
        raiseInfo(id, signum, siCode, siPid, 0);
    }
    restoreInterrupts(flags);
    return anyTarget;
}

uint64 sigSetHandler(Thread* thread, int signum, uint64 handlerAddr, uint64 mask, uint64 flags)
{
    if (signum <= 0 || signum >= sigMaxSignals)
        return sigSetFailed;
    if (signum == sigKill || signum == sigStop)
        return sigSetFailed;

    uint64 previous = thread->sigHandlers[signum];
    thread->sigHandlers[signum] = handlerAddr;

    mask &= ~(1ull << sigKill);
    mask &= ~(1ull << sigStop);
    thread->sigHandlerMasks[signum] = mask;
    thread->sigHandlerFlags[signum] = flags;

    return previous;
}

uint64 sigProcMask(Thread* thread, int how, uint64 mask)
{
    uint64 previous = thread->sigBlocked;
    mask &= ~(1ull << sigKill);
    mask &= ~(1ull << sigStop);

    switch (how) {
    case sigMaskBlock:
        thread->sigBlocked |= mask;
        break;
    case sigMaskUnblock:
        thread->sigBlocked &= ~mask;
        break;
    case sigMaskSetMask:
        thread->sigBlocked = mask;
        break;
    default:
        break;
    }

    return previous;
}

SigOutcome sigConsumePending(Thread* thread, Registers* regs, int* outSignum)
{
    if (thread->sigHandlerDepth >= sigMaxNestDepth)
        return sigOutcomeNone;

    for (int signum = 1; signum < sigMaxSignals; signum++) {
        uint64 bit = 1ull << (uint32)signum;
        if (!(thread->sigPending & bit) || (thread->sigBlocked & bit))
            continue;

        thread->sigPending &= ~bit;
        uint64 handler = thread->sigHandlers[signum];

        if (handler == sigHandlerIgnore ||
            (handler == sigHandlerDefault && sigDefaultIsIgnore(signum)))
            continue;

        if (outSignum)
            *outSignum = signum;

        if (handler == sigHandlerDefault) {
            if (sigDefaultIsStop(signum))
                return sigOutcomeStop;
            return sigOutcomeTerminate;
        }

        bool interruptedRetry = thread->sigInRetryableSyscall;
        thread->sigInRetryableSyscall = false;
        if (interruptedRetry && !(thread->sigHandlerFlags[signum] & sigFlagRestart)) {
            regs->rip += 2;
            regs->rax = (uint64)(int64)errInterrupted;
        }

        uint32 depth = thread->sigHandlerDepth;
        thread->sigSavedRegs[depth] = *regs;
        thread->sigSavedBlocked[depth] = thread->sigBlocked;
        thread->sigHandlerDepth = depth + 1;

        uint64 newBlocked = thread->sigBlocked | thread->sigHandlerMasks[signum];
        if (!(thread->sigHandlerFlags[signum] & sigFlagNoDefer))
            newBlocked |= bit;
        newBlocked &= ~(1ull << sigKill);
        newBlocked &= ~(1ull << sigStop);
        thread->sigBlocked = newBlocked;

        regs->rax = handler;
        regs->rdi = (uint64)signum;
        regs->rip = sigTrampolineVirtualAddr;

        if (thread->sigHandlerFlags[signum] & sigFlagSigInfo && sigInfoPageCount > 0) {
            uint32 slot = thread->id * sigMaxNestDepth + depth;
            uint32 page = slot / sigInfoSlotsPerPage;
            uint32 offset = (slot % sigInfoSlotsPerPage) * (uint32)sizeof(SigInfo);
            SigInfo* dst = (SigInfo*)(sigInfoFrames[page] + offset);
            *dst = thread->sigPendingInfo[signum];
            regs->rsi = sigInfoVirtualAddr + (uint64)page * frameSize + offset;
            regs->rdx = 0;
        }

        return sigOutcomeHandler;
    }

    return sigOutcomeNone;
}

bool sigOnSigReturn(Thread* thread, Registers* regs)
{
    if (thread->sigHandlerDepth == 0)
        return false;

    uint32 depth = thread->sigHandlerDepth - 1;
    *regs = thread->sigSavedRegs[depth];
    thread->sigBlocked = thread->sigSavedBlocked[depth];
    thread->sigHandlerDepth = depth;
    return true;
}

void sigInstallTrampoline(void)
{
    uint64 frame = pmmAlloc();
    if (frame == 0)
        return;

    uint64 length = (uint64)(sigTrampolineEnd - sigTrampolineStart);
    memset((void*)frame, 0, frameSize);
    memcpy((void*)frame, sigTrampolineStart, length);

    pagingMap(sigTrampolineVirtualAddr, frame, pagePresent | pageUser);
}

void sigInstallInfoPage(void)
{
    uint64 totalSlots = (uint64)threadCapacity() * sigMaxNestDepth;
    uint64 pagesNeeded = (totalSlots + sigInfoSlotsPerPage - 1) / sigInfoSlotsPerPage;
    if (pagesNeeded > sigInfoMaxPages)
        pagesNeeded = sigInfoMaxPages;

    for (uint32 i = 0; i < pagesNeeded; i++) {
        uint64 frame = pmmAlloc();
        if (frame == 0)
            return;

        memset((void*)frame, 0, frameSize);
        pagingMap(sigInfoVirtualAddr + (uint64)i * frameSize, frame, pagePresent | pageUser);
        sigInfoFrames[i] = frame;
    }

    sigInfoPageCount = (uint32)pagesNeeded;
}
