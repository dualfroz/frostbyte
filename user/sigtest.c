#include "ulib.h"

int main(void)
{
    print("sigtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userSigSend(999999, sigUsr1) != -1) {
        print("sigtest: FAIL signal to bogus pid should fail\n");
        return 1;
    }
    print("sigtest: signal to bad pid rejected\n");

    if (userSigAction(sigKill, (SigHandler)0, 0, 0) != (long)sigSetFailed) {
        print("sigtest: FAIL installing a sigKill handler should be rejected\n");
        return 1;
    }
    print("sigtest: sigkill-handler-rejected-ok\n");

    long readyHandle = userPipeCreate();
    if (readyHandle < 0) {
        print("sigtest: FAIL create ready pipe\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 0);
    if (readyFd < 0) {
        print("sigtest: FAIL attach ready pipe\n");
        return 1;
    }

    char readyHandleStr[24];
    decToString(readyHandle, readyHandleStr);

    char* targetArgv[] = { "/bin/sigtarget", readyHandleStr, 0 };
    long target = userSpawn("/bin/sigtarget", targetArgv);
    if (target < 0) {
        print("sigtest: FAIL spawn sigtarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("sigtest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    if (userSigSend(target, sigUsr1) != 0) {
        print("sigtest: FAIL send sigUsr1 to sigtarget\n");
        return 1;
    }
    print("sigtest: sent sigUsr1 to sigtarget\n");

    long status = userWait(target);
    print("sigtest: sigtarget exit status=");
    printDec(status);
    print("\n");
    if (status != 42) {
        print("sigtest: FAIL expected sigtarget exit status 42\n");
        return 1;
    }
    print("sigtest: handler-roundtrip-ok\n");

    long defaultReadyHandle = userPipeCreate();
    if (defaultReadyHandle < 0) {
        print("sigtest: FAIL create sigdefault ready pipe\n");
        return 1;
    }
    long defaultReadyFd = userPipeAttach(defaultReadyHandle, 0);
    if (defaultReadyFd < 0) {
        print("sigtest: FAIL attach sigdefault ready pipe\n");
        return 1;
    }

    char defaultReadyHandleStr[24];
    decToString(defaultReadyHandle, defaultReadyHandleStr);
    char* defaultArgv[] = { "/bin/sigdefault", defaultReadyHandleStr, 0 };
    long defaultTarget = userSpawn("/bin/sigdefault", defaultArgv);
    if (defaultTarget < 0) {
        print("sigtest: FAIL spawn sigdefault\n");
        return 1;
    }

    if (userRead((int)defaultReadyFd, &readyByte, 1) != 1) {
        print("sigtest: FAIL read sigdefault ready byte\n");
        return 1;
    }
    userClose((int)defaultReadyFd);

    if (userSigSend(defaultTarget, sigTerm) != 0) {
        print("sigtest: FAIL send sigTerm to sigdefault\n");
        return 1;
    }
    print("sigtest: sent sigTerm to sigdefault\n");

    long defaultStatus = userWait(defaultTarget);
    print("sigtest: sigdefault exit status=");
    printDec(defaultStatus);
    print("\n");
    if (defaultStatus != 128 + sigTerm) {
        print("sigtest: FAIL expected sigdefault exit status 128+sigTerm\n");
        return 1;
    }
    print("sigtest: default-terminate-ok\n");

    return 0;
}
