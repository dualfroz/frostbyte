#include "ulib.h"

/*
 * Only ever spawned by jobtest.c, with two shared pipe handles as argv[1]
 * (the "ready" pipe, this process writes) and argv[2] (the "go" pipe, this
 * process reads) -- decToString'd exactly like fifomaster.c's/sigtarget.c's
 * children. Proves job control (signal.h's SIGSTOP/SIGCONT) works end to
 * end against a thread that is genuinely Blocked mid-syscall, not just
 * Runnable: this process parks in a real blocking userRead on the go pipe,
 * jobtest.c stops it there with a real userSigSend(target, sigStop) (not a
 * kernel-internal shortcut), writes the byte it is waiting for WHILE it is
 * stopped, and only then sends sigCont -- the read can only ever complete
 * through the exact restart-syscall retry loop sysRead already uses
 * (syscall.c), which only runs again once threadContinue has moved this
 * thread back to threadRunnable.
 *
 * No sleep/timing guess anywhere in this file or jobtest.c: every step is
 * ordered by a real blocking read or a real userWait, the same
 * happens-before discipline sigtarget.c's rendezvous pipe already
 * established for a different race.
 */

int main(int argc, char** argv)
{
    print("jobtarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 3) {
        print("jobtarget: FAIL expected two argv (ready handle, go handle)\n");
        return 1;
    }

    long readyHandle = parseDec(argv[1]);
    long goHandle = parseDec(argv[2]);

    long goFd = userPipeAttach(goHandle, 0);
    if (goFd < 0) {
        print("jobtarget: FAIL attach go pipe as reader\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 1);
    if (readyFd < 0) {
        print("jobtarget: FAIL attach ready pipe as writer\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("jobtarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("jobtarget: ready, blocking on go pipe\n");

    char goByte = 0;
    /* Real blocking read: parks this thread (threadBlocked) until a byte is
       actually available. jobtest.c raises SIGSTOP against this exact
       blocking read, then writes the byte, then raises SIGCONT -- the read
       below can only return once BOTH have happened, since sysRead's own
       restart-syscall retry (syscall.c) never runs again while this thread
       is threadStopped instead of threadRunnable. */
    if (userRead((int)goFd, &goByte, 1) != 1) {
        print("jobtarget: FAIL read go byte\n");
        return 1;
    }
    userClose((int)goFd);

    if (goByte != 'g') {
        print("jobtarget: FAIL wrong go byte\n");
        return 1;
    }
    print("jobtarget: received go\n");

    return 55;
}
