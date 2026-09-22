#include "ulib.h"

static volatile int handlerFired = 0;
static volatile int capturedSignum = 0;
static volatile int capturedCode = -1;
static volatile int capturedPid = -1;

static void onUsr1(int signum, SigInfo* info, void* context)
{
    (void)context;
    capturedSignum = signum;
    capturedCode = info->siCode;
    capturedPid = info->siPid;
    handlerFired = 1;
    print("siginfotarget: handler invoked signum=");
    printDec(signum);
    print(" siCode=");
    printDec(info->siCode);
    print(" siPid=");
    printDec(info->siPid);
    print("\n");
}

int main(int argc, char** argv)
{
    print("siginfotarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 3) {
        print("siginfotarget: FAIL expected two argv (pipe handle, parent pid)\n");
        return 1;
    }

    if (userSigActionInfo(sigUsr1, onUsr1, 0, sigFlagSigInfo) == (long)sigSetFailed) {
        print("siginfotarget: FAIL install siginfo handler\n");
        return 1;
    }

    long parentPid = parseDec(argv[2]);

    long handle = parseDec(argv[1]);
    long readyFd = userPipeAttach(handle, 1);
    if (readyFd < 0) {
        print("siginfotarget: FAIL attach ready pipe\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("siginfotarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    print("siginfotarget: waiting for signal\n");

    for (int i = 0; i < 40 && !handlerFired; i++)
        userSleep(20);

    if (!handlerFired) {
        print("siginfotarget: FAIL handler never fired\n");
        return 1;
    }
    if (capturedSignum != sigUsr1) {
        print("siginfotarget: FAIL wrong signum delivered\n");
        return 1;
    }
    if (capturedCode != siCodeUser) {
        print("siginfotarget: FAIL expected siCodeUser\n");
        return 1;
    }
    if (capturedPid != (int)parentPid) {
        print("siginfotarget: FAIL si_pid=");
        printDec(capturedPid);
        print(" does not match real sender pid=");
        printDec(parentPid);
        print("\n");
        return 1;
    }
    print("siginfotarget: siginfo-sender-pid-ok\n");

    print("siginfotarget: resumed-after-siginfo-handler-ok\n");
    return 77;
}
