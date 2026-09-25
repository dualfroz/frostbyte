#include "ulib.h"

static volatile int chldSigCount = 0;

static void onChld(int signum)
{
    (void)signum;
    chldSigCount++;
}

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

    if (chldSigCount != 3) {
        print("stopchldtest: FAIL expected exactly 3 sigchld deliveries, got ");
        printDec(chldSigCount);
        print("\n");
        return 1;
    }

    print("stopchldtest: job-control-sigchld-ok\n");
    return 0;
}
