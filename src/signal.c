#include "signal.h"
#include "string.h"
#include "pmm.h"
#include "paging.h"

/* Machine code for the shared trampoline page, sigTrampoline.S -- position
   independent by construction (no absolute address references at all), so
   copying its bytes anywhere and mapping them there is enough. */
extern char sigTrampolineStart[];
extern char sigTrampolineEnd[];

/* Physical frame addresses backing sigInfoVirtualAddr's page(s), filled in
   by sigInstallInfoPage -- kept as separate frames (not assumed physically
   contiguous, unlike their virtual mapping, which IS contiguous by
   construction: pmmAlloc gives no contiguity guarantee across calls, so
   sigConsumePending must look up the right frame for a given slot rather
   than pointer-arithmetic across a single base). Physical frames below the
   1 GiB boot-time identity map (see sigTrampolineVirtualAddr's own comment)
   are directly dereferenceable through their own physical value from
   kernel code, exactly like sigInstallTrampoline's memcpy into `frame`
   above already relies on. */
static uint64 sigInfoFrames[sigInfoMaxPages];
static uint32 sigInfoPageCount = 0;

/* How many SigInfo structs fit in one frameSize page -- shared by
   sigInstallInfoPage's sizing loop and sigConsumePending's slot lookup, so
   the two can never disagree about the layout. */
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

/* Real-world default disposition for signals this kernel actually defines
   that is Ignore rather than Terminate -- SIGCHLD (no handler installed:
   nothing happens) and SIGCONT (its actual resume effect already happened
   unconditionally in sigRaise -- see file comment -- so "no handler" here
   just means no handler runs, not that nothing happened at all). SIGSTOP
   never reaches this function to begin with (sigRaise bypasses the whole
   scan for it, like sigKill), so it is deliberately absent from this list,
   the same way sigKill already was. SIGTSTP is NOT here -- its default is
   Stop, not Ignore, see sigDefaultIsStop below. Every other defined signal
   defaults to Terminate. */
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

/* Real-world default disposition that is Stop rather than Terminate:
   SIGTSTP only. Unlike SIGSTOP (see file comment / sigRaise), SIGTSTP is
   fully catchable/blockable/ignorable -- this default only applies when it
   is neither, exactly like sigDefaultIsIgnore's entries. */
static bool sigDefaultIsStop(int signum)
{
    return signum == sigTstp;
}

/* Whether `signum` would genuinely be delivered (a real handler OR a
   default disposition of Terminate/Stop -- anything sigConsumePending
   itself would act on, not silently dismiss) if `thread` returned to ring 3
   right now. Deliberately mirrors sigConsumePending's own
   blocked/ignored-dismissal logic for exactly one signal number rather than
   sharing code with it -- sigConsumePending also mutates sigPending/
   sigHandlerDepth/the trapframe as it scans, none of which this pure
   predicate may do, since it runs from sigRaise against a target that is
   not currently resuming at all. Used solely to decide whether a Sleeping/
   Blocked target needs early-waking (threadWakeForSignal, sched.c) for this
   signal -- if this ever drifted out of sync with sigConsumePending's own
   logic, the failure mode is always benign either way: "wakes for a signal
   that turns out not to be delivered" is a harmless spurious wake (the
   target's own retry-syscall loop just re-blocks, exactly like a real
   spurious wakeup), and "does not wake for one that would have delivered"
   just falls back to today's behavior of waiting for a natural wake. */
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

bool sigRaiseInfo(uint32 targetId, int signum, int siCode, int siPid, int siStatus)
{
    if (signum <= 0 || signum >= sigMaxSignals)
        return false;

    /* SIGKILL never touches sigPending/sigBlocked at all -- it is
       delegated straight to the pre-existing unconditional termination
       path, exactly like real kill(pid, SIGKILL) bypassing every handler/
       mask a target could have installed. threadKill does its own id
       validation (range, state), so none is duplicated here. Neither it
       nor SIGSTOP just below ever reaches a sigHandlers[] entry that could
       even be sigFlagSigInfo, so the caller-supplied SigInfo fields are
       simply never used on either of these two paths. */
    if (signum == sigKill)
        return threadKill(targetId);

    /* SIGSTOP: uncatchable, unblockable, unignorable, exactly like SIGKILL
       above -- bypasses sigPending/sigHandlers/sigBlocked entirely and acts
       on ThreadState directly. threadStop does its own id validation. */
    if (signum == sigStop)
        return threadStop(targetId);

    if (targetId == 0 || targetId >= threadCapacity())
        return false;

    Thread* target = threadAt(targetId);
    if (target->state == threadDead || target->state == threadUnused)
        return false;

    /* SIGCONT's resume is likewise not something a mask or a missing
       handler can defer -- called unconditionally before the generic
       pending-bit path below, so a Stopped target is moved back to
       threadRunnable regardless of whether it also has a handler
       installed. Its return value is intentionally ignored: "target
       was not actually Stopped" is a harmless no-op at the process-state
       level (real SIGCONT to a running process), not a reason to skip
       delivering the signal itself for any installed handler. */
    if (signum == sigCont)
        threadContinue(targetId);

    target->sigPending |= (1ull << (uint32)signum);

    /* Attached regardless of whether signum's handler actually has
       sigFlagSigInfo set (sigConsumePending is what decides whether this
       ever gets read at all) -- cheap (4 ints), and simpler than threading
       the handler's own flags in here just to skip it, matching how
       sigHandlerMasks/sigHandlerFlags are likewise set unconditionally by
       sigSetHandler and only ever consulted conditionally. */
    target->sigPendingInfo[signum].siSignum = signum;
    target->sigPendingInfo[signum].siCode = siCode;
    target->sigPendingInfo[signum].siPid = siPid;
    target->sigPendingInfo[signum].siStatus = siStatus;

    /* Real Unix's interruptible sleep/EINTR (deliberately out of scope
       until now, see the file comment's history): a Sleeping/Blocked
       target whose sleep timer or wait-channel condition would otherwise
       never naturally fire again (e.g. user/eintrtarget.c's read that no
       writer will ever satisfy) must still actually receive a signal that
       would genuinely be delivered, not sit on it forever. Skipped
       entirely for a blocked or ignored signal -- see sigWouldDeliver --
       so a masked/ignored signal never disturbs a thread's sleep/wait at
       all, matching real Unix. threadWakeForSignal itself refuses anything
       other than Sleeping/Blocked (Stopped is excluded on purpose -- only
       threadContinue may move it), so this is safe to call unconditionally
       whenever sigWouldDeliver agrees. */
    if (sigWouldDeliver(target, signum))
        threadWakeForSignal(targetId);

    return true;
}

bool sigRaise(uint32 targetId, int signum)
{
    return sigRaiseInfo(targetId, signum, siCodeKernel, 0, 0);
}

bool sigRaiseGroup(uint32 pgid, int signum, int siCode, int siPid)
{
    bool anyTarget = false;
    for (uint32 id = 1; id < threadCapacity(); id++) {
        Thread* candidate = threadAt(id);
        if (candidate->state == threadDead || candidate->state == threadUnused)
            continue;
        if (candidate->pgid != pgid)
            continue;

        anyTarget = true;
        sigRaiseInfo(id, signum, siCode, siPid, 0);
    }
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

    /* Same stripping sigProcMask already does to a mask it is handed: a
       mask cannot make sigKill/sigStop blockable just because it asked. */
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

        /* SA_RESTART / EINTR (see file comment): consumed here, exactly
           once, regardless of which way the decision goes -- a signal
           nested INSIDE the handler this delivery is about to enter is
           interrupting that handler's own code, not a retryable syscall
           retry point, so it must never see this as still true. Only
           rewrites `regs` (the context about to be saved below) when this
           thread was genuinely mid one of sysRead/sysWrite/sysWait's own
           retry-on-block pattern AND this particular signal was not
           registered with sigFlagRestart -- anything else (plain running
           code, a sleep, an already-nested handler, or sigFlagRestart
           itself) leaves `regs` completely untouched, preserving the
           unconditional-retry behavior this kernel had before the flag
           existed. */
        bool interruptedRetry = thread->sigInRetryableSyscall;
        thread->sigInRetryableSyscall = false;
        if (interruptedRetry && !(thread->sigHandlerFlags[signum] & sigFlagRestart)) {
            regs->rip += 2;
            regs->rax = (uint64)(int64)errInterrupted;
        }

        /* Push the whole interrupted trapframe (now possibly rewritten
           above) onto this thread's small context stack (the only copy of
           it that exists anywhere -- unlike a real kernel this never pushes
           one onto the user stack) so sigOnSigReturn can restore it byte
           for byte, even if a DIFFERENT, unmasked signal nests another
           handler on top before this one ever returns. */
        uint32 depth = thread->sigHandlerDepth;
        thread->sigSavedRegs[depth] = *regs;
        thread->sigSavedBlocked[depth] = thread->sigBlocked;
        thread->sigHandlerDepth = depth + 1;

        /* sa_mask (sigHandlerMasks[signum], set by sigSetHandler) plus the
           signal's own bit (self-blocked, like real Unix's implicit
           default) are ADDED to whatever was already blocked -- a union,
           not a replacement, so an outer handler's own blocked set stays
           blocked for this nested one too. sigFlagNoDefer (signalnum.h)
           opts out of exactly the self-block half of that union: a signal
           registered with it does not gain its own bit here, so a second
           raise of the SAME signum while this handler is still active
           finds sigBlocked genuinely clear for it and nests, instead of
           merely staying pending until this handler returns. Stripped of
           sigKill/sigStop for the same reason every other mask operation in
           this file is: neither ever consults sigBlocked at all, so this is
           purely cosmetic, but leaving either bit set here would visibly
           contradict "can never be blocked" for anything that inspects
           sigBlocked. */
        uint64 newBlocked = thread->sigBlocked | thread->sigHandlerMasks[signum];
        if (!(thread->sigHandlerFlags[signum] & sigFlagNoDefer))
            newBlocked |= bit;
        newBlocked &= ~(1ull << sigKill);
        newBlocked &= ~(1ull << sigStop);
        thread->sigBlocked = newBlocked;

        /* Rewrite `regs` in place to enter the shared trampoline: rip is
           the trampoline's entry, rax is the handler address its
           `call *%rax` invokes, rdi is signum (the handler's sole SysV
           argument). regs->rsp (the interrupted user stack) is left
           untouched -- the trampoline runs on the target's own stack,
           right where it already was. */
        regs->rax = handler;
        regs->rdi = (uint64)signum;
        regs->rip = sigTrampolineVirtualAddr;

        /* sigFlagSigInfo (signalnum.h): copy the SigInfo sigRaiseInfo
           attached to this signum into THIS (thread, depth) pair's own
           slot of the shared sigInfoVirtualAddr page -- a different slot
           than any still-active outer handler's own siginfo pointer, by
           the exact same depth-indexed reasoning sigSavedRegs/
           sigSavedBlocked already use, so a nested sigFlagSigInfo delivery
           can never clobber an outer one's still-unread copy. rdx is
           always 0 (no ucontext_t this kernel can expose). Left completely
           untouched otherwise -- see this function's own doc comment. */
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
        /* pageUser, no pageWrite: a handler may only ever READ the SigInfo
           it was handed a pointer to, exactly like the trampoline page
           above is read+execute, never write -- the kernel is the only
           writer, via `frame`'s own physical address (see the frame
           comment above sigInfoFrames), never through this user-facing
           mapping. */
        pagingMap(sigInfoVirtualAddr + (uint64)i * frameSize, frame, pagePresent | pageUser);
        sigInfoFrames[i] = frame;
    }

    sigInfoPageCount = (uint32)pagesNeeded;
}
