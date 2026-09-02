#include "ulib.h"

/*
 * Boot-time driver closing the one gap left open by jobtest.c/jobtarget.c
 * (job control's OTHER live demo): a process stopping ITSELF (self-directed
 * SIGSTOP through sysSigSend, syscall.c) rather than being stopped by a
 * different one. Reasoned about but deliberately left without a live demo
 * for many falas (see include/signal.h's own file comment) because proving
 * it needs a second, independent process to race-freely learn "the target
 * has now actually stopped" before sending SIGCONT -- exactly the kind of
 * guess-free rendezvous sigtarget.c's own pipe-rendezvous comment already
 * had to solve once for a different race. The rendezvous this file uses is
 * sched.c's threadStop already raising a real SIGCHLD (siCodeChldStopped)
 * against the PARENT the instant a child (self- or otherwise) actually
 * transitions to threadStopped -- an event the kernel can only ever raise
 * if threadStop genuinely ran, so a real SigInfo handler catching it here
 * is a zero-guess, race-free signal that selfstoptarget.c's self-stop
 * syscall did not silently no-op.
 *
 * That alone would only prove the STATE flipped, not that execution
 * actually paused -- syscall.c's own comment describes exactly the bug
 * class this demo also has to rule out (a self-stopped thread that falls
 * through to its ordinary return-to-ring3 path instead of explicitly
 * rescheduling would keep running, unnoticed, until the next timer tick).
 * So this file adds a second check the moment the SIGCHLD confirms the
 * stop: a non-blocking (timeoutMs 0) userPoll on the proof pipe
 * selfstoptarget.c writes to only AFTER it resumes -- if that byte were
 * already there, the self-stop never actually paused execution and
 * selfstoptarget raced all the way to writing it before this process's own
 * bounded SIGCHLD-wait loop even noticed. Only once BOTH checks pass does
 * this send the real sigCont.
 */

static volatile int stopFired = 0;
static volatile int stopPid = -1;
static volatile int stopStatus = -1;

static void onChld(int signum, SigInfo* info, void* context)
{
    (void)signum;
    (void)context;
    if (info->siCode != siCodeChldStopped)
        return;
    stopPid = info->siPid;
    stopStatus = info->siStatus;
    stopFired = 1;
}

int main(void)
{
    print("selfstoptest: start, pid=");
    printDec(userGetpid());
    print("\n");

    /* Installed BEFORE selfstoptarget even exists -- threadStop can raise
       the real SIGCHLD the instant selfstoptarget's self-stop syscall
       runs, which could be very soon after userSpawn returns below. */
    if (userSigActionInfo(sigChld, onChld, 0, sigFlagSigInfo) == (long)sigSetFailed) {
        print("selfstoptest: FAIL install siginfo sigchld handler\n");
        return 1;
    }

    long proofHandle = userPipeCreate();
    if (proofHandle < 0) {
        print("selfstoptest: FAIL create proof pipe\n");
        return 1;
    }

    /* Attach as reader before selfstoptarget exists, the same "attach
       first" reason sigtest.c/jobtest.c's own comments already document at
       length -- whichever side attaches second risks the pipe's slot
       already being freed by pipeReleaseIfUnused. */
    long proofFd = userPipeAttach(proofHandle, 0);
    if (proofFd < 0) {
        print("selfstoptest: FAIL attach proof pipe as reader\n");
        return 1;
    }

    char proofHandleStr[24];
    decToString(proofHandle, proofHandleStr);

    char* targetArgv[] = { "/bin/selfstoptarget", proofHandleStr, 0 };
    long target = userSpawn("/bin/selfstoptarget", targetArgv);
    if (target < 0) {
        print("selfstoptest: FAIL spawn selfstoptarget\n");
        return 1;
    }

    /* Bounded poll for the real, kernel-raised SIGCHLD, same idiom
       siginfotest.c/sigchldtest.c already use for the same reason (no
       timing guess, just enough sleep-wake cycles for the return-to-ring3
       pending-signal scan, isr.c, to notice it whenever it actually
       lands). */
    for (int i = 0; i < 80 && !stopFired; i++)
        userSleep(20);

    if (!stopFired) {
        print("selfstoptest: FAIL sigchld-on-self-stop handler never fired\n");
        return 1;
    }
    if (stopPid != (int)target) {
        print("selfstoptest: FAIL si_pid=");
        printDec(stopPid);
        print(" does not match real child pid=");
        printDec(target);
        print("\n");
        return 1;
    }
    if (stopStatus != sigStop) {
        print("selfstoptest: FAIL si_status did not report sigStop\n");
        return 1;
    }
    print("selfstoptest: self-stop-confirmed-via-sigchld-ok\n");

    /* Non-blocking: must return immediately either way (timeoutMs 0), and
       must report the proof pipe as NOT ready -- selfstoptarget.c only
       ever writes to it AFTER resuming from its own self-stop syscall,
       which has not happened yet (sigCont is not sent until after this
       check). A self-stop that silently failed to actually pause execution
       would have let selfstoptarget race ahead and write this byte well
       before this process's own 80-iteration SIGCHLD-wait loop above even
       finished, making this the one check that would catch that exact
       regression. */
    PollFd proofPoll;
    proofPoll.fd = (int)proofFd;
    proofPoll.events = pollIn;
    proofPoll.revents = 12345;
    long ready = userPoll(&proofPoll, 1, 0);
    if (ready != 0 || proofPoll.revents != 0) {
        print("selfstoptest: FAIL proof pipe already had data before sigCont "
              "(self-stop did not actually pause execution)\n");
        return 1;
    }
    print("selfstoptest: still-stopped-before-cont-ok\n");

    if (userSigSend(target, sigCont) != 0) {
        print("selfstoptest: FAIL send sigCont to selfstoptarget\n");
        return 1;
    }
    print("selfstoptest: sent sigCont to selfstoptarget\n");

    /* onChld is installed WITHOUT sigFlagRestart (it needs to fire once,
       immediately, for the stop notification above -- a restarting handler
       would transparently retry, hiding the exact event this file is
       trying to observe). That means it is ALSO live for a second, later
       SIGCHLD: selfstoptarget's own exit, which can land at almost any
       real time after sigCont resumes it. If that exit's SIGCHLD arrives
       while this thread is genuinely parked mid the blocking userRead just
       below, sigConsumePending (signal.c) rewrites the interrupted
       context to return errInterrupted (-4) instead of letting the
       retry-on-block loop run and find the byte selfstoptarget already
       wrote -- the exact same self-signal-vs-own-blocking-syscall race
       stopchldtest.c's own comment documents and retries past (see that
       file for the full reasoning); a correctly written caller with a
       non-restarting handler for the same signal its own blocking syscall
       is waiting on must loop past EINTR the same way. */
    char proofByte = 0;
    long readStatus;
    for (;;) {
        readStatus = userRead((int)proofFd, &proofByte, 1);
        if (readStatus != errInterrupted)
            break;
    }
    if (readStatus != 1) {
        print("selfstoptest: FAIL read proof byte\n");
        return 1;
    }
    userClose((int)proofFd);
    if (proofByte != 'p') {
        print("selfstoptest: FAIL wrong proof byte\n");
        return 1;
    }
    print("selfstoptest: post-cont-proof-received-ok\n");

    /* Same EINTR hazard as the read above, in case the exit SIGCHLD is
       still unconsumed by the time this runs instead. */
    long status;
    for (;;) {
        status = userWait(target);
        if (status != errInterrupted)
            break;
    }
    print("selfstoptest: selfstoptarget exit status=");
    printDec(status);
    print("\n");
    if (status != 66) {
        print("selfstoptest: FAIL expected selfstoptarget exit status 66\n");
        return 1;
    }
    print("selfstoptest: self-stop-continue-ok\n");

    return 0;
}
