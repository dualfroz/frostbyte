#include "ulib.h"

/*
 * Only ever spawned by masktest.c, with three shared pipe handles as argv
 * (ready/handlerReady/go, decToString'd -- the exact same convention
 * nesttarget.c already established, and for the same reason: an extra
 * pipe beyond the usual two is needed to announce "the handler has
 * genuinely entered and is about to block" from INSIDE a signal handler,
 * not main()).
 *
 * Proves the OPPOSITE half of nesttarget.c's story for real, ring-3
 * delivery (signal.h's per-signal sa_mask): unlike nesttarget's onUsr1
 * (mask 0, so sigUsr2 genuinely nests), THIS onUsr1 installs sa_mask =
 * sigUsr2's bit, so sigUsr2 arriving while onUsr1 is active must stay
 * pending, NOT nest -- sigTest.c's
 * sigConsumePendingSaMaskDefersDifferentSignalUntilHandlerReturns already
 * proves this deterministically by driving sigConsumePending/sigOnSigReturn
 * directly; this is its live ring-3 counterpart, plus the one thing a pure
 * logic test cannot show: sigUsr2, once deferred, is actually delivered
 * the INSTANT onUsr1 returns -- riding the very same trap-return sysSigReturn
 * itself takes (isr.c's isrDispatch calls deliverPendingSignals
 * unconditionally right after syscallDispatch handles sysSigReturn), with
 * no further syscall or scheduling needed to notice it. No sleep/timing
 * guess anywhere: the three pipes make "handler installed", "handler
 * entered, mask active, about to block", and "safe to unblock it" all real
 * happens-before rendezvous, the same discipline nesttarget.c/jobtest.c
 * already established for different races.
 */

static volatile int handlerUsr1Entered = 0;
static volatile int handlerUsr2Fired = 0;
static volatile int stillMaskedCheckOk = 0;

static long handlerReadyFd = -1;
static long goFd = -1;

static void onUsr2(int signum)
{
    (void)signum;
    /* Only ever expected to run once onUsr1 has already returned and
       lifted its sa_mask's grip on sigUsr2 -- see the check inside onUsr1
       below for what catches it firing any earlier than that. */
    print("masktarget: usr2-handler-fired-after-unmask\n");
    handlerUsr2Fired = 1;
}

static void onUsr1(int signum)
{
    (void)signum;
    print("masktarget: usr1-handler-enter\n");
    handlerUsr1Entered = 1;

    /* Announces "I am about to block on goFd, with sigUsr2 now masked" --
       masktest.c will not send sigUsr2 until it reads this byte, so
       sigUsr2 can never race ahead of onUsr1's sa_mask actually being in
       effect. */
    if (userWrite((int)handlerReadyFd, "h", 1) != 1) {
        print("masktarget: FAIL write handler-ready byte\n");
        return;
    }
    userClose((int)handlerReadyFd);

    char goByte = 0;
    /* Real blocking read, issued BY THE HANDLER ITSELF, exactly like
       nesttarget.c's own goFd read -- the moment it completes, control
       returns to ring 3 through the same int 0x80 path any syscall does,
       which isr.c's deliverPendingSignals always re-checks. Unlike
       nesttarget.c, sigUsr2 is NOT deliverable here: onUsr1's own sa_mask
       still covers it, so this return-to-ring3 must dismiss it and resume
       onUsr1's own code, not enter a nested handler. */
    if (userRead((int)goFd, &goByte, 1) != 1) {
        print("masktarget: FAIL read go byte inside handler\n");
        return;
    }
    userClose((int)goFd);

    /* The actual proof: if sigUsr2 had genuinely nested (mask ineffective),
       onUsr2 would have already run and set this flag BEFORE control ever
       reached here (a nested handler's own sigreturn is what lets THIS
       read call's return value reach onUsr1's C code at all) -- so seeing
       it still 0 here is not a guess, it is the direct consequence of
       what deliverPendingSignals did (or did not do) at the read's own
       return-to-ring3 point. */
    if (handlerUsr2Fired) {
        print("masktarget: FAIL usr2 fired while masked by usr1's sa_mask\n");
        return;
    }
    stillMaskedCheckOk = 1;
    print("masktarget: still-masked-after-read-ok\n");

    /* Returning from here is sysSigReturn -- sigOnSigReturn restores
       sigBlocked to what it was before onUsr1 (no sigUsr2 bit), and
       isrDispatch's deliverPendingSignals immediately re-runs against the
       just-restored context, in the SAME trap return, before this thread
       ever resumes anywhere else -- sigUsr2, still pending since masktest
       sent it, is delivered right here as its own top-level handler. */
}

int main(int argc, char** argv)
{
    print("masktarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 4) {
        print("masktarget: FAIL expected three argv (ready/handlerReady/go handles)\n");
        return 1;
    }

    long readyHandle = parseDec(argv[1]);
    long handlerReadyHandle = parseDec(argv[2]);
    long goHandle = parseDec(argv[3]);

    /* sigUsr1's sa_mask covers sigUsr2 -- this is the whole point of this
       demo, the opposite of nesttarget.c's mask-0 onUsr1. sigUsr2 itself
       has no mask (nothing is meant to interrupt IT here). sigUsr1's own
       delivery always interrupts main()'s plain userSleep loop below
       (never a retryable syscall), and sigUsr2 never interrupts a
       retryable syscall of its own either (it fires on a sigreturn's own
       trap-return, not mid a sysRead/sysWrite/sysWait retry), so sa_flags
       is irrelevant for both. */
    if (userSigAction(sigUsr1, onUsr1, (1ull << sigUsr2), 0) == (long)sigSetFailed ||
        userSigAction(sigUsr2, onUsr2, 0, 0) == (long)sigSetFailed) {
        print("masktarget: FAIL install handlers\n");
        return 1;
    }

    /* Attach the pipes THIS process will use from inside a handler before
       ever installing/receiving anything that could reach them -- same
       "attach first" discipline sigtest.c/jobtest.c/nesttarget.c already
       document at length, so neither slot can be reclaimed out from under
       a late attach. */
    handlerReadyFd = userPipeAttach(handlerReadyHandle, 1);
    goFd = userPipeAttach(goHandle, 0);
    if (handlerReadyFd < 0 || goFd < 0) {
        print("masktarget: FAIL attach handler-ready/go pipes\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 1);
    if (readyFd < 0) {
        print("masktarget: FAIL attach ready pipe\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("masktarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("masktarget: ready, waiting for sigUsr1\n");

    /* Several short sleeps rather than one long one -- this kernel only
       checks for a pending signal when the target thread actually returns
       to ring 3 (isr.c's deliverPendingSignals), the same reasoning
       nesttarget.c's own poll loop documents. Once onUsr1 fires, ALL the
       real work (the deferred check, then the deferred-until-now onUsr2)
       happens synchronously before it ever returns here. */
    for (int i = 0; i < 80 && !handlerUsr2Fired; i++)
        userSleep(20);

    if (!handlerUsr1Entered) {
        print("masktarget: FAIL usr1 handler never fired\n");
        return 1;
    }
    if (!stillMaskedCheckOk) {
        print("masktarget: FAIL usr1 handler did not confirm usr2 stayed masked\n");
        return 1;
    }
    if (!handlerUsr2Fired) {
        print("masktarget: FAIL usr2 handler never fired after usr1 returned\n");
        return 1;
    }
    print("masktarget: mask-defers-then-delivers-ok\n");

    /* Reached only if BOTH sysSigReturn calls (onUsr1's own, then the
       deferred onUsr2's) correctly restored their respective saved
       contexts -- a corrupted pop at either depth would have crashed,
       hung, or resumed the wrong saved trapframe instead of returning
       here from main()'s own interrupted sleep. */
    print("masktarget: resumed-after-deferred-usr2-ok\n");
    return 78;
}
