#include "ulib.h"

static volatile int handlerUsr1Entered = 0;
static volatile int handlerUsr2Fired = 0;
static volatile int stillMaskedCheckOk = 0;

static long handlerReadyFd = -1;
static long goFd = -1;

static void onUsr2(int signum)
{
    (void)signum;
    print("masktarget: usr2-handler-fired-after-unmask\n");
    handlerUsr2Fired = 1;
}

static void onUsr1(int signum)
{
    (void)signum;
    print("masktarget: usr1-handler-enter\n");
    handlerUsr1Entered = 1;

    if (userWrite((int)handlerReadyFd, "h", 1) != 1) {
        print("masktarget: FAIL write handler-ready byte\n");
        return;
    }
    userClose((int)handlerReadyFd);

    char goByte = 0;
    if (userRead((int)goFd, &goByte, 1) != 1) {
        print("masktarget: FAIL read go byte inside handler\n");
        return;
    }
    userClose((int)goFd);

    if (handlerUsr2Fired) {
        print("masktarget: FAIL usr2 fired while masked by usr1's sa_mask\n");
        return;
    }
    stillMaskedCheckOk = 1;
    print("masktarget: still-masked-after-read-ok\n");

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

    if (userSigAction(sigUsr1, onUsr1, (1ull << sigUsr2), 0) == (long)sigSetFailed ||
        userSigAction(sigUsr2, onUsr2, 0, 0) == (long)sigSetFailed) {
        print("masktarget: FAIL install handlers\n");
        return 1;
    }

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

    print("masktarget: resumed-after-deferred-usr2-ok\n");
    return 78;
}
