#include "ulib.h"

/*
 * Only ever spawned by stopchldtest.c, with two shared pipe handles as
 * argv[1] (the "ready" pipe, this process writes) and argv[2] (the "go"
 * pipe, this process reads) -- identical shape to user/jobtarget.c, reused
 * here so stopchldtest.c can stop/continue a target genuinely Blocked
 * mid-syscall while ALSO watching for the new SIGCHLD-on-stop/
 * SIGCHLD-on-continue notifications (sched.c's threadStop/threadContinue)
 * against itself, something jobtest.c/jobtarget.c never needed since
 * neither side there installs a sigChld handler. A distinct exit code (66,
 * not jobtarget.c's 55) keeps the two pairs' needles/asserts from ever
 * being confusable in the serial log.
 */

int main(int argc, char** argv)
{
    print("stopchldtarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 3) {
        print("stopchldtarget: FAIL expected two argv (ready handle, go handle)\n");
        return 1;
    }

    long readyHandle = parseDec(argv[1]);
    long goHandle = parseDec(argv[2]);

    long goFd = userPipeAttach(goHandle, 0);
    if (goFd < 0) {
        print("stopchldtarget: FAIL attach go pipe as reader\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 1);
    if (readyFd < 0) {
        print("stopchldtarget: FAIL attach ready pipe as writer\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("stopchldtarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("stopchldtarget: ready, blocking on go pipe\n");

    char goByte = 0;
    /* Real blocking read: parks this thread (threadBlocked) until a byte is
       actually available. stopchldtest.c raises SIGSTOP against this exact
       blocking read, waits for its own SIGCHLD-on-stop notification, writes
       the byte, raises SIGCONT, waits for its own SIGCHLD-on-continue
       notification, and only then this read can return -- the same
       restart-syscall retry contract user/jobtarget.c already relies on. */
    if (userRead((int)goFd, &goByte, 1) != 1) {
        print("stopchldtarget: FAIL read go byte\n");
        return 1;
    }
    userClose((int)goFd);

    if (goByte != 'g') {
        print("stopchldtarget: FAIL wrong go byte\n");
        return 1;
    }
    print("stopchldtarget: received go\n");

    return 66;
}
