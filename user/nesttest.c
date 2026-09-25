#include "ulib.h"

int main(void)
{
    print("nesttest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long readyHandle = userPipeCreate();
    long handlerReadyHandle = userPipeCreate();
    long goHandle = userPipeCreate();
    if (readyHandle < 0 || handlerReadyHandle < 0 || goHandle < 0) {
        print("nesttest: FAIL create pipes\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 0);
    long handlerReadyFd = userPipeAttach(handlerReadyHandle, 0);
    long goFd = userPipeAttach(goHandle, 1);
    if (readyFd < 0 || handlerReadyFd < 0 || goFd < 0) {
        print("nesttest: FAIL attach pipes\n");
        return 1;
    }

    char readyHandleStr[24];
    char handlerReadyHandleStr[24];
    char goHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    decToString(handlerReadyHandle, handlerReadyHandleStr);
    decToString(goHandle, goHandleStr);

    char* targetArgv[] = { "/bin/nesttarget", readyHandleStr,
                            handlerReadyHandleStr, goHandleStr, 0 };
    long target = userSpawn("/bin/nesttarget", targetArgv);
    if (target < 0) {
        print("nesttest: FAIL spawn nesttarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("nesttest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("nesttest: nesttarget ready\n");

    if (userSigSend(target, sigUsr1) != 0) {
        print("nesttest: FAIL send sigUsr1 to nesttarget\n");
        return 1;
    }
    print("nesttest: sent sigUsr1 to nesttarget\n");

    char handlerReadyByte;
    if (userRead((int)handlerReadyFd, &handlerReadyByte, 1) != 1) {
        print("nesttest: FAIL read handler-ready byte\n");
        return 1;
    }
    userClose((int)handlerReadyFd);
    print("nesttest: nesttarget usr1-handler-entered\n");

    if (userSigSend(target, sigUsr2) != 0) {
        print("nesttest: FAIL send sigUsr2 to nesttarget\n");
        return 1;
    }
    print("nesttest: sent sigUsr2 to nesttarget\n");

    if (userWrite((int)goFd, "g", 1) != 1) {
        print("nesttest: FAIL write go byte\n");
        return 1;
    }
    userClose((int)goFd);

    long status = userWait(target);
    print("nesttest: nesttarget exit status=");
    printDec(status);
    print("\n");
    if (status != 77) {
        print("nesttest: FAIL expected nesttarget exit status 77\n");
        return 1;
    }
    print("nesttest: nested-signal-delivery-ok\n");

    return 0;
}
