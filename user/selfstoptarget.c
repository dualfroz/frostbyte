#include "ulib.h"

/*
 * Only ever spawned by selfstoptest.c, with argv[1] = a shared pipe handle
 * (userPipeCreate'd by selfstoptest.c, decToString'd exactly like every
 * other *test / *target pair's own shared-handle argv) this process attaches
 * as the WRITER end.
 *
 * Unlike jobtest.c/jobtarget.c (job control's other live demo), the SIGSTOP
 * here targets THIS process's OWN pid -- self-directed job control
 * (syscall.c's sysSigSend, the "selfTargeted" branch right after
 * sigRaise/threadStop) rather than one process stopping a different one.
 * No ready-pipe rendezvous is needed before the self-stop itself: unlike
 * jobtest.c's own sigStop (sent from a DIFFERENT process, which could
 * otherwise race ahead of jobtarget's own setup), this process is the only
 * one that can ever call self-stop, so there is nothing else to order it
 * against -- it simply self-stops the moment it has attached the pipe.
 *
 * selfstoptest.c's own real proof that this process is genuinely Stopped
 * (not still running, not merely eventually preempted by the next timer
 * tick) is entirely on ITS side (a real SIGCHLD siCodeChldStopped
 * notification, then a non-blocking poll proving the pipe below is still
 * empty) -- this process only needs to write its own proof byte AFTER
 * actually resuming, which can only happen once selfstoptest.c's own
 * userSigSend(this pid, sigCont) runs.
 */

int main(int argc, char** argv)
{
    print("selfstoptarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 2) {
        print("selfstoptarget: FAIL expected one argv (proof pipe handle)\n");
        return 1;
    }

    long handle = parseDec(argv[1]);
    long proofFd = userPipeAttach(handle, 1);
    if (proofFd < 0) {
        print("selfstoptarget: FAIL attach proof pipe as writer\n");
        return 1;
    }

    print("selfstoptarget: self-stopping now\n");

    /* Self-targeted SIGSTOP: syscall.c's sysSigSend recognizes rawPid ==
       senderPid (this process's own pid) as "selfTargeted", and after
       threadStop (called from sigRaise above) flips this thread's own
       state to threadStopped, explicitly reschedules instead of just
       falling through to its normal return-to-ring3 path -- the exact gap
       syscall.c's own comment documents (a self-stopped thread that simply
       returned to ring 3 would keep running, unnoticed, until the next
       timer tick). Does not return until a real
       userSigSend(this pid, sigCont) from selfstoptest.c moves this thread
       back to threadRunnable and the scheduler actually picks it again. */
    if (userSigSend(userGetpid(), sigStop) != 0) {
        print("selfstoptarget: FAIL self-directed sigStop rejected\n");
        return 1;
    }

    print("selfstoptarget: resumed-after-self-stop-ok\n");

    if (userWrite((int)proofFd, "p", 1) != 1) {
        print("selfstoptarget: FAIL write proof byte\n");
        return 1;
    }
    userClose((int)proofFd);

    return 66;
}
