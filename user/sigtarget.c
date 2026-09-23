#include "ulib.h"

static volatile int handlerFired = 0;
static volatile int handlerSignum = 0;

static void onUsr1(int signum)
{
    handlerFired = 1;
    handlerSignum = signum;
    char signumText[24];
    decToString(signum, signumText);
    printParts((const char* const[]){ "sigtarget: handler invoked signum=", signumText, "\n", 0 });
}

int main(int argc, char** argv)
{
    print("sigtarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 2) {
        print("sigtarget: FAIL expected one argv (pipe handle)\n");
        return 1;
    }

    if (userSigAction(sigUsr1, onUsr1, 0, 0) == (long)sigSetFailed) {
        print("sigtarget: FAIL install handler\n");
        return 1;
    }

    long handle = parseDec(argv[1]);
    long readyFd = userPipeAttach(handle, 1);
    if (readyFd < 0) {
        print("sigtarget: FAIL attach ready pipe\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("sigtarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    print("sigtarget: waiting for signal\n");

    for (int i = 0; i < 40 && !handlerFired; i++)
        userSleep(20);

    if (!handlerFired) {
        print("sigtarget: FAIL handler never fired\n");
        return 1;
    }
    if (handlerSignum != sigUsr1) {
        print("sigtarget: FAIL wrong signum delivered\n");
        return 1;
    }
    print("sigtarget: handler-fired-ok\n");

    print("sigtarget: resumed-after-handler-ok\n");
    return 42;
}
