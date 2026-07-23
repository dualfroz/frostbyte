#include "ulib.h"

/*
 * Only ever spawned by restarttest.c, with four shared pipe handles as argv
 * (ready/announce1/announce2/data, decToString'd -- the same convention
 * nesttarget.c/jobtarget.c already establish for a multi-pipe rendezvous).
 *
 * Proves signal.h's SA_RESTART/EINTR distinction (signalnum.h's
 * sigFlagRestart, sched.h's Thread.sigInRetryableSyscall) end to end, in
 * two phases against two independently registered handlers:
 *
 *   phase 1 (no sigFlagRestart): blocks in a real userRead on a pipe NO ONE
 *   will EVER write (the exact "blocked forever" setup user/eintrtarget.c
 *   already uses for threadKill/early-wake), gets sigUsr1 -- registered
 *   WITHOUT sigFlagRestart -- and must see that read return errInterrupted
 *   the instant onUsr1 returns, not silently retry and hang forever.
 *
 *   phase 2 (WITH sigFlagRestart): blocks in a real userRead on a SECOND
 *   pipe restarttest.c genuinely does write, gets sigUsr2 -- registered
 *   WITH sigFlagRestart -- while blocked, and must see that read
 *   transparently resume and return the real byte once onUsr2 returns,
 *   exactly like every other retryable syscall already did before this
 *   flag existed (user/nesttarget.c's onUsr1/goFd relies on precisely this
 *   for an unrelated reason -- see that file's own comment).
 *
 * Each phase's blocking read is announced ("about to call it now") over
 * its own pipe immediately beforehand, the same "announce, then the OTHER
 * process learns it via its own blocking read before acting" discipline
 * nesttest.c/nesttarget.c already established for the analogous nested-
 * delivery race -- not airtight against an adversarial scheduler in
 * principle, but the SAME assumption those already-verified demos (and
 * eintrtest.c's) rely on in practice. The rigorous, race-free proof of the
 * actual rip/rax rewrite lives in sigTest.c, driving sigConsumePending
 * directly.
 */

static volatile int handlerUsr1Fired = 0;
static volatile int handlerUsr2Fired = 0;

static void onUsr1(int signum)
{
    (void)signum;
    handlerUsr1Fired = 1;
    print("restarttarget: usr1-handler-fired\n");
}

static void onUsr2(int signum)
{
    (void)signum;
    handlerUsr2Fired = 1;
    print("restarttarget: usr2-handler-fired\n");
}

int main(int argc, char** argv)
{
    print("restarttarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 5) {
        print("restarttarget: FAIL expected four argv (ready/announce1/announce2/data handles)\n");
        return 1;
    }

    /* sigUsr1 deliberately has NO sigFlagRestart (0): phase 1 below expects
       its own interrupted read to return errInterrupted, not resume.
       sigUsr2 deliberately DOES have sigFlagRestart: phase 2 expects the
       opposite, a transparent resume with the real data. */
    if (userSigAction(sigUsr1, onUsr1, 0, 0) == (long)sigSetFailed ||
        userSigAction(sigUsr2, onUsr2, 0, sigFlagRestart) == (long)sigSetFailed) {
        print("restarttarget: FAIL install handlers\n");
        return 1;
    }

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("restarttarget: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("restarttarget: FAIL attach victim pipe\n");
        return 1;
    }

    long readyHandle = parseDec(argv[1]);
    long announce1Handle = parseDec(argv[2]);
    long announce2Handle = parseDec(argv[3]);
    long dataHandle = parseDec(argv[4]);

    long readyFd = userPipeAttach(readyHandle, 1);
    long announce1Fd = userPipeAttach(announce1Handle, 1);
    long announce2Fd = userPipeAttach(announce2Handle, 1);
    long dataFd = userPipeAttach(dataHandle, 0);
    if (readyFd < 0 || announce1Fd < 0 || announce2Fd < 0 || dataFd < 0) {
        print("restarttarget: FAIL attach ready/announce/data pipes\n");
        return 1;
    }

    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("restarttarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("restarttarget: ready, handlers installed\n");

    /* Phase 1: no-restart. */
    print("restarttarget: phase1 blocking on victim read (expect errInterrupted)\n");
    if (userWrite((int)announce1Fd, "a", 1) != 1) {
        print("restarttarget: FAIL write announce1 byte\n");
        return 1;
    }
    userClose((int)announce1Fd);

    char victimBuffer[4];
    long n1 = userRead((int)victimFd, victimBuffer, sizeof(victimBuffer));
    userClose((int)victimFd);

    if (!handlerUsr1Fired) {
        print("restarttarget: FAIL usr1 handler never fired\n");
        return 1;
    }
    if (n1 != errInterrupted) {
        print("restarttarget: FAIL victim read did not return errInterrupted\n");
        return 1;
    }
    print("restarttarget: phase1-eintr-no-restart-ok\n");

    /* Phase 2: sigFlagRestart. */
    print("restarttarget: phase2 blocking on data read (expect transparent restart)\n");
    if (userWrite((int)announce2Fd, "a", 1) != 1) {
        print("restarttarget: FAIL write announce2 byte\n");
        return 1;
    }
    userClose((int)announce2Fd);

    char dataByte = 0;
    long n2 = userRead((int)dataFd, &dataByte, 1);
    userClose((int)dataFd);

    if (!handlerUsr2Fired) {
        print("restarttarget: FAIL usr2 handler never fired\n");
        return 1;
    }
    if (n2 != 1 || dataByte != 'd') {
        print("restarttarget: FAIL data read did not transparently resume with real data\n");
        return 1;
    }
    print("restarttarget: phase2-restart-transparent-ok\n");

    print("restarttarget: all-phases-ok\n");
    return 55;
}
