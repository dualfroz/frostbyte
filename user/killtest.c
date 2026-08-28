#include "ulib.h"

/*
 * Exercises threadKill (sched.c) end to end through sysKill: spawns
 * killvictim.c (which blocks forever in a real, syscall-level pipe read
 * with no writer ever attaching), waits long enough for it to actually
 * reach and park inside that read, then kills it and confirms userWait
 * reports exactly the killed exit status (9) -- the only way this kernel
 * can ever recover a process stuck in a genuine block, now that
 * sysRead/sysWrite can really block (pipe.h/vfs.h, same wave as this).
 */

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

    long victim = userSpawn("/bin/killvictim", 0);
    if (victim < 0) {
        print("killtest: FAIL spawn killvictim\n");
        return 1;
    }

    /* Give the victim time to actually attach and reach its blocking read
       -- killing it before it ever calls userRead would still "work" (any
       Runnable/Sleeping/Blocked thread is a valid target), but would not
       prove the interesting case: tearing down a call already parked
       mid-syscall. */
    userSleep(100);

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

    /* Already dead and reaped (userWait above reaped it): killing it again
       must fail cleanly, not resurrect a slot that has since been reused. */
    if (userKill(victim) != -1) {
        print("killtest: FAIL kill of already-reaped pid should fail\n");
        return 1;
    }

    print("killtest: kill-ok\n");
    return 0;
}
