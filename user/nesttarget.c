#include "ulib.h"

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

    if (userWrite((int)handlerReadyFd, "h", 1) != 1) {
        print("nesttarget: FAIL write handler-ready byte\n");
        return;
    }
    userClose((int)handlerReadyFd);

    char goByte = 0;
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

    if (userSigAction(sigUsr1, onUsr1, 0, 0) == (long)sigSetFailed ||
        userSigAction(sigUsr2, onUsr2, 0, sigFlagRestart) == (long)sigSetFailed) {
        print("nesttarget: FAIL install handlers\n");
        return 1;
    }

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

    print("nesttarget: resumed-after-both-handlers-ok\n");
    return 77;
}
