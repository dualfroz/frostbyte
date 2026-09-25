#include "ulib.h"

int main(void)
{
    print("eintrtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long readyHandle = userPipeCreate();
    if (readyHandle < 0) {
        print("eintrtest: FAIL create ready pipe\n");
        return 1;
    }
    long readyFd = userPipeAttach(readyHandle, 0);
    if (readyFd < 0) {
        print("eintrtest: FAIL attach ready pipe\n");
        return 1;
    }

    char readyHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    char* targetArgv[] = { "/bin/eintrtarget", readyHandleStr, 0 };
    long target = userSpawn("/bin/eintrtarget", targetArgv);
    if (target < 0) {
        print("eintrtest: FAIL spawn eintrtarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("eintrtest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("eintrtest: eintrtarget ready\n");

    if (userSigSend(target, sigTerm) != 0) {
        print("eintrtest: FAIL send sigTerm to eintrtarget\n");
        return 1;
    }
    print("eintrtest: sent sigTerm to eintrtarget\n");

    long status = userWait(target);
    print("eintrtest: eintrtarget exit status=");
    printDec(status);
    print("\n");
    if (status != 128 + sigTerm) {
        print("eintrtest: FAIL expected eintrtarget exit status 128+sigTerm\n");
        return 1;
    }
    print("eintrtest: early-wake-signal-ok\n");

    return 0;
}
