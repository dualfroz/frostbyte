#include "ulib.h"

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

    if (userSigActionInfo(sigChld, onChld, 0, sigFlagSigInfo) == (long)sigSetFailed) {
        print("selfstoptest: FAIL install siginfo sigchld handler\n");
        return 1;
    }

    long proofHandle = userPipeCreate();
    if (proofHandle < 0) {
        print("selfstoptest: FAIL create proof pipe\n");
        return 1;
    }

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
