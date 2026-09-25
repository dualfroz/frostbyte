#include "ulib.h"

int main(void)
{
    print("restarttest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long readyHandle = userPipeCreate();
    long announce1Handle = userPipeCreate();
    long announce2Handle = userPipeCreate();
    long dataHandle = userPipeCreate();
    if (readyHandle < 0 || announce1Handle < 0 || announce2Handle < 0 || dataHandle < 0) {
        print("restarttest: FAIL create pipes\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 0);
    long announce1Fd = userPipeAttach(announce1Handle, 0);
    long announce2Fd = userPipeAttach(announce2Handle, 0);
    long dataFd = userPipeAttach(dataHandle, 1);
    if (readyFd < 0 || announce1Fd < 0 || announce2Fd < 0 || dataFd < 0) {
        print("restarttest: FAIL attach pipes\n");
        return 1;
    }

    char readyHandleStr[24];
    char announce1HandleStr[24];
    char announce2HandleStr[24];
    char dataHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    decToString(announce1Handle, announce1HandleStr);
    decToString(announce2Handle, announce2HandleStr);
    decToString(dataHandle, dataHandleStr);

    char* targetArgv[] = { "/bin/restarttarget", readyHandleStr, announce1HandleStr,
                            announce2HandleStr, dataHandleStr, 0 };
    long target = userSpawn("/bin/restarttarget", targetArgv);
    if (target < 0) {
        print("restarttest: FAIL spawn restarttarget\n");
        return 1;
    }

    char byte;
    if (userRead((int)readyFd, &byte, 1) != 1) {
        print("restarttest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("restarttest: restarttarget ready\n");

    if (userRead((int)announce1Fd, &byte, 1) != 1) {
        print("restarttest: FAIL read announce1 byte\n");
        return 1;
    }
    userClose((int)announce1Fd);
    print("restarttest: restarttarget about to block (phase1)\n");

    if (userSigSend(target, sigUsr1) != 0) {
        print("restarttest: FAIL send sigUsr1 to restarttarget\n");
        return 1;
    }
    print("restarttest: sent sigUsr1 (no restart) to restarttarget\n");

    if (userRead((int)announce2Fd, &byte, 1) != 1) {
        print("restarttest: FAIL read announce2 byte\n");
        return 1;
    }
    userClose((int)announce2Fd);
    print("restarttest: restarttarget about to block (phase2)\n");

    if (userSigSend(target, sigUsr2) != 0) {
        print("restarttest: FAIL send sigUsr2 to restarttarget\n");
        return 1;
    }
    print("restarttest: sent sigUsr2 (restart) to restarttarget\n");

    if (userWrite((int)dataFd, "d", 1) != 1) {
        print("restarttest: FAIL write data byte\n");
        return 1;
    }
    userClose((int)dataFd);

    long status = userWait(target);
    print("restarttest: restarttarget exit status=");
    printDec(status);
    print("\n");
    if (status != 55) {
        print("restarttest: FAIL expected restarttarget exit status 55\n");
        return 1;
    }
    print("restarttest: sa-restart-eintr-distinction-ok\n");

    return 0;
}
