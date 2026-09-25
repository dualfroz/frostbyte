#include "ulib.h"

int main(void)
{
    print("waitanytest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long fastPid = userSpawn("/bin/waitanyfast", 0);
    if (fastPid < 0) {
        print("waitanytest: FAIL spawn waitanyfast\n");
        return 1;
    }
    long slowPid = userSpawn("/bin/waitanyslow", 0);
    if (slowPid < 0) {
        print("waitanytest: FAIL spawn waitanyslow\n");
        return 1;
    }

    long reapedPid[2];
    long reapedCode[2];
    for (int i = 0; i < 2; i++) {
        long pid = -1;
        long code = userWaitAny(&pid);
        print("waitanytest: reaped pid=");
        printDec(pid);
        print(" code=");
        printDec(code);
        print("\n");
        if (code < 0) {
            print("waitanytest: FAIL userWaitAny reported no children too early\n");
            return 1;
        }
        reapedPid[i] = pid;
        reapedCode[i] = code;
    }

    int sawFast = 0;
    int sawSlow = 0;
    for (int i = 0; i < 2; i++) {
        if (reapedPid[i] == fastPid) {
            sawFast = 1;
            if (reapedCode[i] != 44) {
                print("waitanytest: FAIL waitanyfast exit code mismatch\n");
                return 1;
            }
        } else if (reapedPid[i] == slowPid) {
            sawSlow = 1;
            if (reapedCode[i] != 33) {
                print("waitanytest: FAIL waitanyslow exit code mismatch\n");
                return 1;
            }
        } else {
            print("waitanytest: FAIL reaped an unrecognized pid\n");
            return 1;
        }
    }
    if (!sawFast || !sawSlow) {
        print("waitanytest: FAIL did not reap both known children exactly once\n");
        return 1;
    }
    print("waitanytest: any-child-reap-set-ok\n");

    long pid = 0;
    long status = userWaitAny(&pid);
    if (status != -1) {
        print("waitanytest: FAIL expected -1 (ECHILD) once no children remain\n");
        return 1;
    }
    print("waitanytest: echild-when-no-children-ok\n");

    return 0;
}
