#include "ulib.h"

/*
 * Only ever spawned by nesttest.c, with three shared pipe handles as argv
 * (ready/handlerReady/go, decToString'd -- the same convention jobtest.c's
 * two-pipe rendezvous already established, extended to three because this
 * needs an extra one to announce "the handler has genuinely entered and is
 * about to block" from INSIDE a signal handler, not main()).
 *
 * Proves real, ring-3 NESTED signal delivery end to end (signal.h's
 * per-signal sa_mask / Thread.sigHandlerDepth): onUsr1 installs no sa_mask
 * at all (mask 0), so sigUsr2 arriving while it is still active is not
 * blocked by anything -- it genuinely interrupts onUsr1 with its own,
 * separate trampoline entry, onUsr1 resuming afterwards exactly where the
 * blocking read below left it. No sleep/timing guess: the three pipes make
 * "handler installed", "handler entered and blocked", and "safe to unblock
 * it" all real happens-before rendezvous, the same discipline sigtarget.c/
 * jobtest.c already established for different races.
 */

static volatile int handlerUsr1Entered = 0;
static volatile int handlerUsr2FiredNested = 0;
static volatile int handlerUsr1Resumed = 0;

static long handlerReadyFd = -1;
static long goFd = -1;

static void onUsr2(int signum)
{
    (void)signum;
    print("nesttarget: usr2-handler-fired-nested\n");
    handlerUsr2FiredNested = 1;
}

static void onUsr1(int signum)
{
    (void)signum;
    print("nesttarget: usr1-handler-enter\n");
    handlerUsr1Entered = 1;

    /* Announces "I am about to block on goFd" from INSIDE the handler --
       nesttest.c will not send sigUsr2 until it reads this byte, so sigUsr2
       can never race ahead of onUsr1 actually being active. */
    if (userWrite((int)handlerReadyFd, "h", 1) != 1) {
        print("nesttarget: FAIL write handler-ready byte\n");
        return;
    }
    userClose((int)handlerReadyFd);

    char goByte = 0;
    /* Real blocking read, issued BY THE HANDLER ITSELF: the moment it
       completes, control returns to ring 3 through the exact same
       int 0x80 path any syscall does, which isr.c's deliverPendingSignals
       always re-checks -- if sigUsr2 is pending and unblocked by then (it
       is: onUsr1's own sa_mask is empty), it is delivered as a genuinely
       NESTED handler right here, before this function's next line ever
       runs. */
    if (userRead((int)goFd, &goByte, 1) != 1) {
        print("nesttarget: FAIL read go byte inside handler\n");
        return;
    }
    userClose((int)goFd);

    print("nesttarget: usr1-handler-resumed-after-nested-usr2\n");
    handlerUsr1Resumed = 1;
}

int main(int argc, char** argv)
{
    print("nesttarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 4) {
        print("nesttarget: FAIL expected three argv (ready/handlerReady/go handles)\n");
        return 1;
    }

    long readyHandle = parseDec(argv[1]);
    long handlerReadyHandle = parseDec(argv[2]);
    long goHandle = parseDec(argv[3]);

    /* sigUsr1's sa_mask is 0 (no additional blocking) so sigUsr2 -- also
       unmasked -- can genuinely nest inside it; sigUsr2 has no mask of its
       own either, since nothing is meant to interrupt IT here. sigUsr1's
       own delivery always interrupts main()'s plain userSleep loop below
       (never a retryable syscall), so its sa_flags is irrelevant either
       way. sigUsr2, though, genuinely interrupts onUsr1's OWN blocking
       userRead(goFd) -- this test's whole point is that read transparently
       resumes and returns the real byte once onUsr2 returns, so sigUsr2
       is registered WITH sigFlagRestart (signalnum.h); without it, that
       read would return errInterrupted instead, which
       user/restarttarget.c -- not this file -- is what actually exercises. */
    if (userSigAction(sigUsr1, onUsr1, 0, 0) == (long)sigSetFailed ||
        userSigAction(sigUsr2, onUsr2, 0, sigFlagRestart) == (long)sigSetFailed) {
        print("nesttarget: FAIL install handlers\n");
        return 1;
    }

    /* Attach the pipes THIS process will use from inside a handler before
       ever installing/receiving anything that could reach them -- same
       "attach first" discipline sigtest.c/jobtest.c document at length, so
       neither slot can be reclaimed out from under a late attach. */
    handlerReadyFd = userPipeAttach(handlerReadyHandle, 1);
    goFd = userPipeAttach(goHandle, 0);
    if (handlerReadyFd < 0 || goFd < 0) {
        print("nesttarget: FAIL attach handler-ready/go pipes\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 1);
    if (readyFd < 0) {
        print("nesttarget: FAIL attach ready pipe\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("nesttarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("nesttarget: ready, waiting for sigUsr1\n");

    /* Several short sleeps rather than one long one -- this kernel only
       checks for a pending signal when the target thread actually returns
       to ring 3 (isr.c's deliverPendingSignals), the same reasoning
       sigtarget.c's own poll loop documents. Once onUsr1 fires, ALL the
       real work (including the nested onUsr2) happens synchronously inside
       it before it ever returns here. */
    for (int i = 0; i < 80 && !handlerUsr1Resumed; i++)
        userSleep(20);

    if (!handlerUsr1Entered) {
        print("nesttarget: FAIL usr1 handler never fired\n");
        return 1;
    }
    if (!handlerUsr2FiredNested) {
        print("nesttarget: FAIL usr2 handler never fired nested inside usr1\n");
        return 1;
    }
    if (!handlerUsr1Resumed) {
        print("nesttarget: FAIL usr1 handler never resumed after nested usr2\n");
        return 1;
    }
    print("nesttarget: nested-delivery-ok\n");

    /* Reached only if BOTH sysSigReturn calls (the nested usr2's and then
       usr1's own) correctly restored their respective saved contexts --
       a corrupted pop at either depth would have crashed, hung, or
       resumed the wrong one of the two saved trapframes instead of
       returning here from onUsr1's own return. */
    print("nesttarget: resumed-after-both-handlers-ok\n");
    return 77;
}
