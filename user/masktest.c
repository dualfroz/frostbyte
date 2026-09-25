#include "ulib.h"

int main(void)
{
    print("masktest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long readyHandle = userPipeCreate();
    long handlerReadyHandle = userPipeCreate();
    long goHandle = userPipeCreate();
    if (readyHandle < 0 || handlerReadyHandle < 0 || goHandle < 0) {
        print("masktest: FAIL create pipes\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 0);
    long handlerReadyFd = userPipeAttach(handlerReadyHandle, 0);
    long goFd = userPipeAttach(goHandle, 1);
    if (readyFd < 0 || handlerReadyFd < 0 || goFd < 0) {
        print("masktest: FAIL attach pipes\n");
        return 1;
    }

    char readyHandleStr[24];
    char handlerReadyHandleStr[24];
    char goHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    decToString(handlerReadyHandle, handlerReadyHandleStr);
    decToString(goHandle, goHandleStr);

    char* targetArgv[] = { "/bin/masktarget", readyHandleStr,
                            handlerReadyHandleStr, goHandleStr, 0 };
    long target = userSpawn("/bin/masktarget", targetArgv);
    if (target < 0) {
        print("masktest: FAIL spawn masktarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("masktest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("masktest: masktarget ready\n");

    if (userSigSend(target, sigUsr1) != 0) {
        print("masktest: FAIL send sigUsr1 to masktarget\n");
        return 1;
    }
    print("masktest: sent sigUsr1 to masktarget\n");

    char handlerReadyByte;
    if (userRead((int)handlerReadyFd, &handlerReadyByte, 1) != 1) {
        print("masktest: FAIL read handler-ready byte\n");
        return 1;
    }
    userClose((int)handlerReadyFd);
    print("masktest: masktarget usr1-handler-entered\n");

    if (userSigSend(target, sigUsr2) != 0) {
        print("masktest: FAIL send sigUsr2 to masktarget\n");
        return 1;
    }
    print("masktest: sent sigUsr2 to masktarget (should stay masked)\n");

    if (userWrite((int)goFd, "g", 1) != 1) {
        print("masktest: FAIL write go byte\n");
        return 1;
    }
    userClose((int)goFd);

    long status = userWait(target);
    print("masktest: masktarget exit status=");
    printDec(status);
    print("\n");
    if (status != 78) {
        print("masktest: FAIL expected masktarget exit status 78\n");
        return 1;
    }
    print("masktest: sa-mask-defer-then-deliver-ok\n");

    return 0;
}
