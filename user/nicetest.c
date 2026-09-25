#include "ulib.h"

int main(void)
{
    print("nicetest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userNice(999999, 2048) != -1) {
        print("nicetest: FAIL nice on bogus pid should fail\n");
        return 1;
    }
    if (userGetPriority(999999) != -1) {
        print("nicetest: FAIL getpriority on bogus pid should fail\n");
        return 1;
    }
    print("nicetest: bogus-pid-rejected-ok\n");

    if (userNice(0, 0) != -1) {
        print("nicetest: FAIL nice with zero weight should fail\n");
        return 1;
    }
    print("nicetest: zero-weight-rejected-ok\n");

    long before = userGetPriority(0);
    if (before <= 0) {
        print("nicetest: FAIL getpriority(self) before renice\n");
        return 1;
    }
    if (userNice(0, before * 2) != 0) {
        print("nicetest: FAIL nice(self) should succeed\n");
        return 1;
    }
    long after = userGetPriority(0);
    if (after != before * 2) {
        print("nicetest: FAIL self weight did not change as expected\n");
        return 1;
    }
    print("nicetest: self-renice-ok\n");

    userNice(0, before);

    long readyHandle = userPipeCreate();
    long goHandle = userPipeCreate();
    if (readyHandle < 0 || goHandle < 0) {
        print("nicetest: FAIL create handshake pipes\n");
        return 1;
    }
    long readyFd = userPipeAttach(readyHandle, 0);
    long goFd = userPipeAttach(goHandle, 1);
    if (readyFd < 0 || goFd < 0) {
        print("nicetest: FAIL attach handshake pipes\n");
        return 1;
    }

    char readyHandleStr[24];
    char goHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    decToString(goHandle, goHandleStr);
    char* targetArgv[] = { "/bin/nicetarget", readyHandleStr, goHandleStr, 0 };
    long target = userSpawn("/bin/nicetarget", targetArgv);
    if (target < 0) {
        print("nicetest: FAIL spawn nicetarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("nicetest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    if (userNice(target, 4096) != 0) {
        print("nicetest: FAIL renice nicetarget while sleeping\n");
        return 1;
    }
    long targetWeight = userGetPriority(target);
    if (targetWeight != 4096) {
        print("nicetest: FAIL getpriority(nicetarget) after renice\n");
        return 1;
    }
    print("nicetest: other-renice-ok\n");

    if (userWrite((int)goFd, "g", 1) != 1) {
        print("nicetest: FAIL write go byte\n");
        return 1;
    }
    userClose((int)goFd);

    long status = userWait(target);
    print("nicetest: nicetarget exit status=");
    printDec(status);
    print("\n");
    if (status != 0) {
        print("nicetest: FAIL nicetarget did not exit cleanly\n");
        return 1;
    }

    if (userNice(target, 1024) != -1) {
        print("nicetest: FAIL renice of already-reaped pid should fail\n");
        return 1;
    }
    print("nicetest: reaped-pid-rejected-ok\n");

    print("nicetest: priority-syscalls-ok\n");
    return 0;
}
