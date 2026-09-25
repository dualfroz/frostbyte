#include "ulib.h"

int main(void)
{
    print("pidfdtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userPidfdOpen(0) != -1) {
        print("pidfdtest: FAIL pidfd on pid 0 should be rejected\n");
        return 1;
    }
    if (userPidfdOpen(999999) != -1) {
        print("pidfdtest: FAIL pidfd on bogus pid should be rejected\n");
        return 1;
    }
    print("pidfdtest: bad-target rejected\n");

    long readyHandle = userPipeCreate();
    if (readyHandle < 0) {
        print("pidfdtest: FAIL create ready pipe\n");
        return 1;
    }
    long readyFd = userPipeAttach(readyHandle, 0);
    if (readyFd < 0) {
        print("pidfdtest: FAIL attach ready pipe\n");
        return 1;
    }

    char readyHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    char* victimArgv[] = { "/bin/pidfdvictim", readyHandleStr, 0 };
    long victim = userSpawn("/bin/pidfdvictim", victimArgv);
    if (victim < 0) {
        print("pidfdtest: FAIL spawn pidfdvictim\n");
        return 1;
    }

    long pfd = userPidfdOpen(victim);
    if (pfd < 0) {
        print("pidfdtest: FAIL pidfd open\n");
        return 1;
    }
    print("pidfdtest: pidfd-open-ok\n");

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("pidfdtest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    if (userPidfdSend(pfd, sigKill) != 0) {
        print("pidfdtest: FAIL pidfd send sigKill\n");
        return 1;
    }
    print("pidfdtest: pidfd-send-ok\n");

    long status = userPidfdWait(pfd);
    print("pidfdtest: victim exit status=");
    printDec(status);
    print("\n");
    if (status != 9) {
        print("pidfdtest: FAIL expected exit status 9\n");
        return 1;
    }
    print("pidfdtest: pidfd-wait-ok\n");

    if (userPidfdSend(pfd, sigUsr1) != -1) {
        print("pidfdtest: FAIL send on reaped pidfd should fail\n");
        return 1;
    }
    if (userPidfdWait(pfd) != -1) {
        print("pidfdtest: FAIL wait on reaped pidfd should fail\n");
        return 1;
    }
    print("pidfdtest: reaped-target-detected-ok\n");

    userClose((int)pfd);
    print("pidfdtest: PASS\n");
    return 0;
}
