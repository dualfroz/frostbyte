#include "ulib.h"

/*
 * Boot-time driver for real, kernel-generated SIGCHLD (sched.h's new
 * Thread.parentId, threadExit/threadKill's new sigRaise(parentId, sigChld)
 * call): proves the kernel itself notifies a process's SPAWNING parent the
 * instant a child it created via sysSpawn actually dies, asynchronously,
 * without that parent ever having to call sysWait first to find out.
 *
 * eintrtest.c already proves early-wake delivery works generically for an
 * arbitrary signal explicitly sent at a controlled time; proctest.c
 * already proves sysWait/threadJoin's synchronous "block until this pid is
 * Dead" contract. Neither proves the ONE thing new here: that the kernel
 * itself, unprompted, raises a specific signal (sigChld) against a
 * specific target (the spawning parent, not just "whoever calls wait")
 * from exactly the right lifecycle event (threadExit/threadKill).
 *
 * Uses the same bounded "check a flag every userSleep(20)" poll idiom
 * masktarget.c/nesttarget.c already use for their own outer verification,
 * deliberately NOT eintrtarget.c's tight write-then-block-forever shape:
 * sigchldchild.c's death is not explicitly time-controlled by any
 * userSigSend call the way eintrtest.c's sigTerm or masktest.c's sigUsr1/
 * sigUsr2 are, so there is no readiness rendezvous to build here that
 * would not itself just be racing the same autonomous kernel event it is
 * trying to order against. The poll loop sidesteps that entirely: whether
 * sigchldchild.c dies before or after any given iteration starts, the very
 * next natural sleep-wake cycle's return to ring 3 re-checks pending
 * signals (isr.c's deliverPendingSignals, unconditional on every return),
 * so the pending sigChld bit is picked up regardless of ordering -- no
 * timing guess, just a different (still real, still ring-3) rendezvous
 * shape than the tight-block one.
 */

static volatile int handlerFired = 0;

static void onChld(int signum)
{
    (void)signum;
    print("sigchldtest: sigchld-handler-fired\n");
    handlerFired = 1;
}

int main(void)
{
    print("sigchldtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userSigAction(sigChld, onChld, 0, 0) == (long)sigSetFailed) {
        print("sigchldtest: FAIL install sigchld handler\n");
        return 1;
    }

    long child = userSpawn("/bin/sigchldchild", 0);
    if (child < 0) {
        print("sigchldtest: FAIL spawn sigchldchild\n");
        return 1;
    }

    for (int i = 0; i < 80 && !handlerFired; i++)
        userSleep(20);

    if (!handlerFired) {
        print("sigchldtest: FAIL sigchld handler never fired\n");
        return 1;
    }
    print("sigchldtest: sigchld-delivered-to-parent-ok\n");

    long status = userWait(child);
    print("sigchldtest: sigchldchild exit status=");
    printDec(status);
    print("\n");
    if (status != 84) {
        print("sigchldtest: FAIL expected sigchldchild exit status 84\n");
        return 1;
    }
    print("sigchldtest: chld-reap-after-signal-ok\n");

    return 0;
}
