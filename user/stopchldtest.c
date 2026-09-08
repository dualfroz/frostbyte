#include "ulib.h"

/*
 * Boot-time driver for the new SIGCHLD-on-stop/SIGCHLD-on-continue
 * notification (sched.c's threadStop/threadContinue, mirroring
 * threadExit/threadKill's existing sigChld-on-death contract): proves the
 * kernel itself notifies a process's parent, asynchronously, both when a
 * child it spawned actually stops AND when it later resumes -- not only
 * when it eventually dies (user/sigchldtest.c already covers death;
 * user/jobtest.c already covers stop/continue genuinely working against a
 * thread blocked mid-syscall, but installs no sigChld handler at all).
 * This is the first live demo combining both: a real installed handler
 * (sigchldtest.c's shape) watching a target genuinely Blocked in its own
 * syscall (jobtest.c's shape) across THREE distinct lifecycle events in a
 * single run.
 *
 * sigChld's pending bit (signal.c) is a single flag, not a queue -- two
 * raises before the handler ever runs would coalesce into one delivery.
 * This is why each stage below waits (the same bounded "check a counter
 * every userSleep(20)" poll idiom sigchldtest.c/masktarget.c/nesttarget.c
 * already use) for its own handler firing to be OBSERVED before triggering
 * the next event: stop, confirm, write+cont, confirm, wait, confirm --
 * proving all three notifications actually land distinctly rather than
 * silently collapsing into fewer than three.
 *
 * Two independent pipes (ready, go), attached before spawning, for the
 * same "attach first" race-avoidance reason user/jobtest.c's own file
 * comment documents at length.
 */

static volatile int chldSigCount = 0;

static void onChld(int signum)
{
    (void)signum;
    chldSigCount++;
}

/* Bounded poll for chldSigCount reaching `want`, exactly the idiom
   user/sigchldtest.c's own file comment explains: whether the kernel event
   already happened or is about to, the next natural sleep-wake cycle's
   return to ring 3 re-checks pending signals unconditionally (isr.c's
   deliverPendingSignals), so this never has to guess timing. */
static int waitForChldSigCount(int want)
{
    for (int i = 0; i < 80 && chldSigCount < want; i++)
        userSleep(20);
    return chldSigCount >= want;
}

int main(void)
{
    print("stopchldtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userSigAction(sigChld, onChld, 0, 0) == (long)sigSetFailed) {
        print("stopchldtest: FAIL install sigchld handler\n");
        return 1;
    }

    long readyHandle = userPipeCreate();
    long goHandle = userPipeCreate();
    if (readyHandle < 0 || goHandle < 0) {
        print("stopchldtest: FAIL create pipes\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 0);
    long goFd = userPipeAttach(goHandle, 1);
    if (readyFd < 0 || goFd < 0) {
        print("stopchldtest: FAIL attach pipes\n");
        return 1;
    }

    char readyHandleStr[24];
    char goHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    decToString(goHandle, goHandleStr);

    char* targetArgv[] = { "/bin/stopchldtarget", readyHandleStr, goHandleStr, 0 };
    long target = userSpawn("/bin/stopchldtarget", targetArgv);
    if (target < 0) {
        print("stopchldtest: FAIL spawn stopchldtarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("stopchldtest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("stopchldtest: stopchldtarget ready\n");

    if (userSigSend(target, sigStop) != 0) {
        print("stopchldtest: FAIL send sigStop to stopchldtarget\n");
        return 1;
    }
    print("stopchldtest: sent sigStop to stopchldtarget\n");

    if (!waitForChldSigCount(1)) {
        print("stopchldtest: FAIL sigchld-on-stop never fired\n");
        return 1;
    }
    print("stopchldtest: sigchld-on-stop-ok\n");

    /* Same "write while stopped" proof as jobtest.c: the byte the target is
       blocked on queues up behind a genuinely Stopped thread without being
       lost or delivered early. */
    if (userWrite((int)goFd, "g", 1) != 1) {
        print("stopchldtest: FAIL write go byte\n");
        return 1;
    }

    if (userSigSend(target, sigCont) != 0) {
        print("stopchldtest: FAIL send sigCont to stopchldtarget\n");
        return 1;
    }
    print("stopchldtest: sent sigCont to stopchldtarget\n");
    userClose((int)goFd);

    if (!waitForChldSigCount(2)) {
        print("stopchldtest: FAIL sigchld-on-continue never fired\n");
        return 1;
    }
    print("stopchldtest: sigchld-on-continue-ok\n");

    /* onChld is installed WITHOUT sigFlagRestart (see above), so this
       userWait is itself one of the three retryable syscalls sigConsumePending
       (signal.c) can hijack: if stopchldtarget's own death (and the sigChld
       it raises against US, the exact signal this handler is for) happens
       WHILE this thread is genuinely parked mid userWait's retry-on-block
       pattern, delivery does not politely wait its turn behind the retry --
       it REWRITES the interrupted context to return errInterrupted right
       then, exactly like restarttarget.c's own phase 1 proves for an
       unrelated signal/syscall pair, and the retry that would have found
       the now-dead child never gets to run at all. A real, correctly
       written POSIX program with a non-restarting SIGCHLD handler has this
       exact same hazard against its own wait()/waitpid() and handles it the
       same way: loop past EINTR. Only the FAST path (target already Dead
       by the time this syscall's body first runs, no parking needed at
       all) returns 66 on the first try; whichever way it goes, the retry
       (if one was needed) always finds the child already reaped-or-dead,
       so this never loops more than twice in practice. */
    long status;
    for (;;) {
        status = userWait(target);
        if (status != errInterrupted)
            break;
    }
    print("stopchldtest: stopchldtarget exit status=");
    printDec(status);
    print("\n");
    if (status != 66) {
        print("stopchldtest: FAIL expected stopchldtarget exit status 66\n");
        return 1;
    }

    /* Whether userWait above needed one retry or returned 66 immediately,
       onChld has by now fired exactly a third time: either it ran AS the
       interruption above (incrementing the count before rewriting status
       to errInterrupted, so the retry's own later success finds it already
       at 3), or it ran as a genuine nested delivery riding the same
       successful return home (the fast-path case) -- sigChld's pending bit
       is a single flag either way, so exactly one of those two shapes
       happens per death, never both, never neither. This checks the count
       is EXACTLY 3, not just at least 3, proving stop+continue+exit
       produced three distinct notifications rather than two of them
       silently coalescing into one pending bit somewhere along the way. */
    if (chldSigCount != 3) {
        print("stopchldtest: FAIL expected exactly 3 sigchld deliveries, got ");
        printDec(chldSigCount);
        print("\n");
        return 1;
    }

    print("stopchldtest: job-control-sigchld-ok\n");
    return 0;
}
