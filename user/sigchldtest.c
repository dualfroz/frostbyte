#include "ulib.h"

static volatile int handlerFired = 0;

static void onChld(int signum)
{
    (void)signum;
    print("sigchldtest: sigchld-handler-fired\n");
    handlerFired = 1;
}

int main(void)
{
    print("sigchldtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userSigAction(sigChld, onChld, 0, 0) == (long)sigSetFailed) {
        print("sigchldtest: FAIL install sigchld handler\n");
        return 1;
    }

    long child = userSpawn("/bin/sigchldchild", 0);
    if (child < 0) {
        print("sigchldtest: FAIL spawn sigchldchild\n");
        return 1;
    }

    for (int i = 0; i < 80 && !handlerFired; i++)
        userSleep(20);

    if (!handlerFired) {
        print("sigchldtest: FAIL sigchld handler never fired\n");
        return 1;
    }
    print("sigchldtest: sigchld-delivered-to-parent-ok\n");

    long status = userWait(child);
    print("sigchldtest: sigchldchild exit status=");
    printDec(status);
    print("\n");
    if (status != 84) {
        print("sigchldtest: FAIL expected sigchldchild exit status 84\n");
        return 1;
    }
    print("sigchldtest: chld-reap-after-signal-ok\n");

    return 0;
}
