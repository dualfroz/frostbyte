#include "ulib.h"

int main(void)
{
    print("proctest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long child = userSpawn("/bin/spawntarget", 0);
    if (child < 0) {
        print("proctest: FAIL spawn /bin/spawntarget\n");
        return 1;
    }

    long status = userWait(child);
    if (status != 3) {
        print("proctest: FAIL wait returned unexpected status\n");
        return 1;
    }
    print("proctest: spawn+wait ok\n");

    if (userSpawn("/bin/does-not-exist", 0) != -1) {
        print("proctest: FAIL spawn of missing binary should fail\n");
        return 1;
    }
    print("proctest: spawn bad path rejected\n");

    if (userSpawn("/bin/proctest", 0) != spawnSlotBusy) {
        print("proctest: FAIL spawn of a live binary should report spawnSlotBusy\n");
        return 1;
    }
    print("proctest: spawn of live binary rejected\n");

    if (userWait(999999) != -1) {
        print("proctest: FAIL wait on bogus pid should fail immediately\n");
        return 1;
    }
    print("proctest: wait bad pid rejected\n");

    char* argtestArgv[] = { "/bin/argtest", "alpha", "beta", "gamma", 0 };
    long argChild = userSpawn("/bin/argtest", argtestArgv);
    if (argChild < 0) {
        print("proctest: FAIL spawn /bin/argtest\n");
        return 1;
    }

    long argStatus = userWait(argChild);
    if (argStatus != 0) {
        print("proctest: FAIL argtest reported bad argv\n");
        return 1;
    }
    print("proctest: spawn with argv ok\n");

    long liveTarget = userSpawn("/bin/exectarget", 0);
    if (liveTarget < 0) {
        print("proctest: FAIL spawn /bin/exectarget\n");
        return 1;
    }
    if (userExec("/bin/exectarget", 0) != spawnSlotBusy) {
        print("proctest: FAIL exec of a live binary should report spawnSlotBusy\n");
        return 1;
    }
    if (userWait(liveTarget) != 7) {
        print("proctest: FAIL exectarget child reported an unexpected status\n");
        return 1;
    }
    print("proctest: exec of live binary rejected\n");

    userExec("/bin/exectarget", 0);

    print("proctest: FAIL exec did not take over\n");
    return 1;
}
