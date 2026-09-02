#include "ulib.h"

/*
 * Boot-time driver for real SA_SIGINFO delivery (signalnum.h's
 * sigFlagSigInfo/SigInfo, signal.c's sigConsumePending) -- proves the
 * extra (SigInfo*, void*) arguments a sigFlagSigInfo handler receives
 * carry genuinely accurate data, not just a valid-looking pointer, in BOTH
 * directions this kernel ever populates SigInfo from:
 *
 *   1. si_pid on a user-sent signal (sysSigSend, syscall.c): spawns
 *      siginfotarget.c and sends it a real sigUsr1 through userSigSend;
 *      siginfotarget's own handler verifies the si_pid it received is
 *      genuinely THIS process's pid (passed as an argv string so there is
 *      no ambiguity about what "correct" means), not just some nonzero
 *      value that happened to be lying around.
 *
 *   2. si_code/si_pid/si_status on a real, kernel-raised SIGCHLD
 *      (threadExit, sched.c): installs its OWN sigFlagSigInfo handler for
 *      sigChld before ever spawning siginfotarget (the same race sigchldtest.c
 *      already had to close with a pre-spawn install), then -- once
 *      siginfotarget's own round trip above completes and it exits with a
 *      distinctive code (77) -- verifies the real SIGCHLD its own handler
 *      received reports siCodeChldExited, the CHILD's real pid, and the
 *      CHILD's real exit status, exactly matching what userWait's
 *      synchronous reap independently confirms right after.
 *
 * Reuses siginfotarget.c's own ready-pipe rendezvous (sigtest.c's pattern)
 * for the same reason sigtest.c needed it: sigUsr1's default disposition
 * is Terminate, so sending it before siginfotarget has actually installed
 * its handler would kill it instead of proving anything.
 */

static volatile int chldFired = 0;
static volatile int chldCode = -1;
static volatile int chldPid = -1;
static volatile int chldStatus = -1;

static void onChld(int signum, SigInfo* info, void* context)
{
    (void)signum;
    (void)context;
    chldCode = info->siCode;
    chldPid = info->siPid;
    chldStatus = info->siStatus;
    chldFired = 1;
    print("siginfotest: sigchld-info-handler-fired siCode=");
    printDec(info->siCode);
    print(" siPid=");
    printDec(info->siPid);
    print(" siStatus=");
    printDec(info->siStatus);
    print("\n");
}

int main(void)
{
    print("siginfotest: start, pid=");
    printDec(userGetpid());
    print("\n");

    /* Installed BEFORE siginfotarget even exists, exactly like
       sigchldtest.c's own onChld -- a real SIGCHLD can arrive the instant
       siginfotarget dies, which this process has no other way to order
       itself ahead of. */
    if (userSigActionInfo(sigChld, onChld, 0, sigFlagSigInfo) == (long)sigSetFailed) {
        print("siginfotest: FAIL install siginfo sigchld handler\n");
        return 1;
    }

    long readyHandle = userPipeCreate();
    if (readyHandle < 0) {
        print("siginfotest: FAIL create ready pipe\n");
        return 1;
    }

    /* Attach as reader before siginfotarget exists -- same detached-slot
       race sigtest.c's own comment documents avoiding. */
    long readyFd = userPipeAttach(readyHandle, 0);
    if (readyFd < 0) {
        print("siginfotest: FAIL attach ready pipe\n");
        return 1;
    }

    char readyHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    char ownPidStr[24];
    decToString(userGetpid(), ownPidStr);

    char* targetArgv[] = { "/bin/siginfotarget", readyHandleStr, ownPidStr, 0 };
    long target = userSpawn("/bin/siginfotarget", targetArgv);
    if (target < 0) {
        print("siginfotest: FAIL spawn siginfotarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("siginfotest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    if (userSigSend(target, sigUsr1) != 0) {
        print("siginfotest: FAIL send sigUsr1 to siginfotarget\n");
        return 1;
    }
    print("siginfotest: sent sigUsr1 to siginfotarget\n");

    /* Bounded poll for the real, kernel-raised SIGCHLD, same idiom
       sigchldtest.c already uses for the same reason (no timing guess,
       just enough sleep-wake cycles for the return-to-ring3 pending-signal
       scan, isr.c, to notice it whenever it actually lands). */
    for (int i = 0; i < 80 && !chldFired; i++)
        userSleep(20);

    if (!chldFired) {
        print("siginfotest: FAIL sigchld siginfo handler never fired\n");
        return 1;
    }
    if (chldCode != siCodeChldExited) {
        print("siginfotest: FAIL expected siCodeChldExited\n");
        return 1;
    }
    if (chldPid != (int)target) {
        print("siginfotest: FAIL si_pid=");
        printDec(chldPid);
        print(" does not match real child pid=");
        printDec(target);
        print("\n");
        return 1;
    }
    if (chldStatus != 77) {
        print("siginfotest: FAIL si_status=");
        printDec(chldStatus);
        print(" does not match real child exit code 77\n");
        return 1;
    }
    print("siginfotest: sigchld-siginfo-accurate-ok\n");

    long status = userWait(target);
    print("siginfotest: siginfotarget exit status=");
    printDec(status);
    print("\n");
    if (status != 77) {
        print("siginfotest: FAIL expected siginfotarget exit status 77\n");
        return 1;
    }
    print("siginfotest: siginfo-roundtrip-ok\n");

    return 0;
}
