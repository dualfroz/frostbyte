#include "ulib.h"

/*
 * Boot-time driver for waitpid(-1)-style userWaitAny (ulib.c/sched.c's new
 * threadJoinAnyChild) -- proves a parent can reap TWO independently exiting
 * children without ever naming either pid, in whichever order they
 * actually die, and correctly reports ECHILD (-1, no blocking) once none
 * are left.
 *
 * Spawns waitanyfast.c (exits immediately) then waitanyslow.c (sleeps
 * 150ms first), then calls userWaitAny() twice:
 *   - the first call happens essentially immediately after both spawns, so
 *     waitanyslow.c cannot possibly be Dead yet (its own 150ms sleep alone
 *     dwarfs the handful of syscalls in between) -- it must be
 *     waitanyfast.c that gets reaped here, exercising
 *     threadJoinAnyChild's waitScanFoundDead branch without ever blocking.
 *   - the second call therefore has only waitanyslow.c left, which is
 *     genuinely still alive at that point -- it must take the
 *     waitScanNoneDead branch and actually block through sysWait's real
 *     retry-syscall path (schedBlockCurrent(currentThread()), sched.c's
 *     new any-child wake in threadExit) until waitanyslow.c's sleep
 *     elapses and it exits.
 * Verified without leaning on that ordering being guaranteed, though: both
 * reaped pids together must equal exactly {fastPid, slowPid} as a set, and
 * each one's own exit code must match whichever pid it actually was, in
 * whatever order they came back.
 * A third userWaitAny() call, once both children are gone, must return -1
 * immediately (ECHILD) rather than hang forever with no children left to
 * wait for -- proves threadJoinAnyChild's waitScanNoChildren branch live.
 */

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
