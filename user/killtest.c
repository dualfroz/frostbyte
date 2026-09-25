#include "ulib.h"

int main(void)
{
    print("killtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userKill(userGetpid()) != -1) {
        print("killtest: FAIL self-kill should be rejected\n");
        return 1;
    }
    print("killtest: self-kill rejected\n");

    if (userKill(999999) != -1) {
        print("killtest: FAIL kill of bogus pid should fail\n");
        return 1;
    }
    print("killtest: kill bad pid rejected\n");

    long readyHandle = userPipeCreate();
    if (readyHandle < 0) {
        print("killtest: FAIL create ready pipe\n");
        return 1;
    }
    long readyFd = userPipeAttach(readyHandle, 0);
    if (readyFd < 0) {
        print("killtest: FAIL attach ready pipe\n");
        return 1;
    }

    char readyHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    char* victimArgv[] = { "/bin/killvictim", readyHandleStr, 0 };
    long victim = userSpawn("/bin/killvictim", victimArgv);
    if (victim < 0) {
        print("killtest: FAIL spawn killvictim\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("killtest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    if (userKill(victim) != 0) {
        print("killtest: FAIL kill victim\n");
        return 1;
    }
    print("killtest: killed victim\n");

    long status = userWait(victim);
    print("killtest: victim exit status=");
    printDec(status);
    print("\n");
    if (status != 9) {
        print("killtest: FAIL expected exit status 9\n");
        return 1;
    }

    if (userKill(victim) != -1) {
        print("killtest: FAIL kill of already-reaped pid should fail\n");
        return 1;
    }

    print("killtest: kill-ok\n");
    return 0;
}
