#include "ulib.h"

static volatile int handlerFired = 0;

static void onUsr1(int signum)
{
    (void)signum;
    handlerFired = 1;
    print("pgroupbg: still-alive-after-ctrlc-ok\n");
}

int main(void)
{
    long myPid = userGetpid();
    print("pgroupbg: start, pid=");
    printDec(myPid);
    print("\n");

    long pgid = userGetpgid(0);
    if (pgid != myPid) {
        print("pgroupbg: FAIL expected to be its own group leader\n");
        return 1;
    }
    if (userSetpgid(0, 0) != 0) {
        print("pgroupbg: FAIL self setpgid(0,0) rejected\n");
        return 1;
    }
    if (userGetpgid(0) != myPid) {
        print("pgroupbg: FAIL pgid changed after self-affirming setpgid\n");
        return 1;
    }
    print("pgroupbg: own-process-group-ok\n");

    if (userSigAction(sigUsr1, onUsr1, 0, 0) == (long)sigSetFailed) {
        print("pgroupbg: FAIL install handler\n");
        return 1;
    }

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("pgroupbg: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("pgroupbg: FAIL attach victim pipe\n");
        return 1;
    }

    print("pgroupbg: ready, blocking on go pipe\n");
    char buffer[4];
    userRead((int)victimFd, buffer, sizeof(buffer));

    if (!handlerFired) {
        print("pgroupbg: FAIL read returned without being signaled\n");
        return 1;
    }

    print("pgroupbg: resumed-after-signal-ok\n");
    return 0;
}
