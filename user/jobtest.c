#include "ulib.h"

int main(void)
{
    print("jobtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long readyHandle = userPipeCreate();
    long goHandle = userPipeCreate();
    if (readyHandle < 0 || goHandle < 0) {
        print("jobtest: FAIL create pipes\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 0);
    long goFd = userPipeAttach(goHandle, 1);
    if (readyFd < 0 || goFd < 0) {
        print("jobtest: FAIL attach pipes\n");
        return 1;
    }

    char readyHandleStr[24];
    char goHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    decToString(goHandle, goHandleStr);

    char* targetArgv[] = { "/bin/jobtarget", readyHandleStr, goHandleStr, 0 };
    long target = userSpawn("/bin/jobtarget", targetArgv);
    if (target < 0) {
        print("jobtest: FAIL spawn jobtarget\n");
        return 1;
    }

    if (userGetpgid(target) != userGetpgid(0)) {
        print("jobtest: FAIL jobtarget did not inherit parent pgid\n");
        return 1;
    }
    print("jobtest: child-inherits-parent-pgid-ok\n");

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("jobtest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("jobtest: jobtarget ready\n");

    if (userSigSend(target, sigStop) != 0) {
        print("jobtest: FAIL send sigStop to jobtarget\n");
        return 1;
    }
    print("jobtest: sent sigStop to jobtarget\n");

    if (userWrite((int)goFd, "g", 1) != 1) {
        print("jobtest: FAIL write go byte\n");
        return 1;
    }

    if (userSigSend(target, sigCont) != 0) {
        print("jobtest: FAIL send sigCont to jobtarget\n");
        return 1;
    }
    print("jobtest: sent sigCont to jobtarget\n");
    userClose((int)goFd);

    long status = userWait(target);
    print("jobtest: jobtarget exit status=");
    printDec(status);
    print("\n");
    if (status != 55) {
        print("jobtest: FAIL expected jobtarget exit status 55\n");
        return 1;
    }
    print("jobtest: job-stop-continue-ok\n");

    return 0;
}
