#include "ulib.h"

static volatile int handlerFired = 0;

static void onUsr1(int signum)
{
    (void)signum;
    handlerFired = 1;
    print("jobctlbg: still-alive-after-ctrlc-ok\n");
}

int main(void)
{
    long myPid = userGetpid();
    print("jobctlbg: start, pid=");
    printDec(myPid);
    print("\n");

    long pgid = userGetpgid(0);
    if (pgid != myPid) {
        print("jobctlbg: FAIL expected to be its own group leader\n");
        return 1;
    }
    print("jobctlbg: own-process-group-ok\n");

    if (userSigAction(sigUsr1, onUsr1, 0, 0) == (long)sigSetFailed) {
        print("jobctlbg: FAIL install handler\n");
        return 1;
    }

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("jobctlbg: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("jobctlbg: FAIL attach victim pipe\n");
        return 1;
    }

    print("jobctlbg: ready, blocking on go pipe\n");
    char buffer[4];
    userRead((int)victimFd, buffer, sizeof(buffer));

    if (!handlerFired) {
        print("jobctlbg: FAIL read returned without being signaled\n");
        return 1;
    }

    print("jobctlbg: resumed-after-signal-ok\n");
    return 0;
}
