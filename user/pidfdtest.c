#include "ulib.h"

/*
 * Exercises pidfd.h end to end through the real syscall path
 * (sysPidfdOpen/sysPidfdSend/sysPidfdWait) -- the fd-table-wrapped,
 * generation-checked handle sched.c's Thread.generation and pidfd.c add
 * as an ADDITIVE alternative to a raw pid, mirroring real Linux's own
 * pidfd_open(2) (see pidfd.h's own file comment for the full design).
 * Reuses killvictim.c (blocks forever in a real, syscall-level pipe read
 * with no writer ever attaching, see killtest.c) as its target -- the
 * same already-proven "genuinely stuck, only killable from outside"
 * shape, no new victim binary needed. Fully synchronous like
 * nicetest.c/date.c/timetest.c: no injected keystrokes, no shared shell
 * channel, so there is no scheduling-timing race to flake on regardless
 * of host load.
 */

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

    long victim = userSpawn("/bin/killvictim", 0);
    if (victim < 0) {
        print("pidfdtest: FAIL spawn killvictim\n");
        return 1;
    }

    long pfd = userPidfdOpen(victim);
    if (pfd < 0) {
        print("pidfdtest: FAIL pidfd open\n");
        return 1;
    }
    print("pidfdtest: pidfd-open-ok\n");

    /* Give the victim time to actually attach and reach its blocking read
       -- same reasoning as killtest.c's own identical sleep. */
    userSleep(100);

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

    /* Already reaped by the wait above: a further send/wait on the SAME
       handle must cleanly report the target as gone, not silently act on
       whichever thread (if any) now occupies that exact numeric slot --
       the whole reason pidfd.h exists (see its own file comment) over
       caching victim's raw pid and calling userSigSend(victim, ...)/
       userWait(victim) again, which killtest.c's own comment already
       documents as unsafe once a slot is recycled. */
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
