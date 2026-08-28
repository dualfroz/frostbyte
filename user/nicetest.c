#include "ulib.h"

/*
 * Boot-time driver for sysNice/sysGetPriority (syscall.c), the first way
 * for any real userspace process to reach sched.h's CFS-lite scheduling
 * weight -- until this fala, threadSetPriority was reachable only from
 * kmain.c itself, once, to set up the kernel-internal workerA/workerB
 * "higher weight runs more often" demo (sched-priority-ok's own needle).
 * That demo already proves the SCHEDULER honors weight; this one proves
 * the SYSCALL PLUMBING that lets userspace change it does the right thing:
 * pid==0 self-targeting, targeting a real separate process (including one
 * that is genuinely Sleeping, not just Runnable), and every rejection path
 * (bogus pid, zero weight, an already-reaped pid) sysKill's own
 * killtest.c already established the shape for.
 *
 * Fully synchronous, like cwdtest.c/cwdtarget.c: every step is ordered by
 * a real userSleep/userWait, never an injected keystroke or a pipe
 * rendezvous, so unlike most of this kernel's other live demos this one
 * has no scheduling-timing race to flake on at all.
 */

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

    /* Restore the original weight before moving on -- nothing downstream
       depends on it, just honest hygiene rather than leaving this process
       double-weighted for the rest of its own run. */
    userNice(0, before);

    long target = userSpawn("/bin/nicetarget", 0);
    if (target < 0) {
        print("nicetest: FAIL spawn nicetarget\n");
        return 1;
    }

    /* Give nicetarget time to actually reach its own sleep before touching
       it -- same reasoning as killtest.c's own userSleep(100) before
       killing its victim mid-syscall. */
    userSleep(100);

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

    long status = userWait(target);
    print("nicetest: nicetarget exit status=");
    printDec(status);
    print("\n");
    if (status != 0) {
        print("nicetest: FAIL nicetarget did not exit cleanly\n");
        return 1;
    }

    /* Already dead and reaped (userWait above reaped it): renicing it
       again must fail cleanly, not resurrect a slot that has since been
       reused -- same "kill an already-reaped pid" shape killtest.c's own
       last check has. */
    if (userNice(target, 1024) != -1) {
        print("nicetest: FAIL renice of already-reaped pid should fail\n");
        return 1;
    }
    print("nicetest: reaped-pid-rejected-ok\n");

    print("nicetest: priority-syscalls-ok\n");
    return 0;
}
