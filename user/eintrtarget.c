#include "ulib.h"

/*
 * Only ever spawned by eintrtest.c. Attaches as the READ end of its own,
 * self-contained pipe (userPipeCreate/userPipeAttach) that no writer will
 * ever attach to -- the exact same "blocked forever" setup killtest.c's
 * killvictim.c already uses for threadKill -- but the way out here is an
 * ORDINARY signal (sigTerm, default disposition Terminate) going through
 * the FULL sigRaise -> pending-bit -> early-wake -> deliverPendingSignals
 * pipeline (signal.h/sched.c's threadWakeForSignal), not threadKill's
 * direct ThreadState bypass. Before that early-wake existed, sigRaise only
 * ever set a pending bit and left a Blocked thread exactly where it was --
 * delivery only happens at a return-to-ring3 this process would otherwise
 * never naturally reach again, so sigTerm sent here would have done
 * nothing observable, ever: the read below is never naturally satisfied,
 * by construction.
 */

int main(int argc, char** argv)
{
    print("eintrtarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 2) {
        print("eintrtarget: FAIL expected one argv (ready handle)\n");
        return 1;
    }

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("eintrtarget: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("eintrtarget: FAIL attach victim pipe\n");
        return 1;
    }

    long readyHandle = parseDec(argv[1]);
    long readyFd = userPipeAttach(readyHandle, 1);
    if (readyFd < 0) {
        print("eintrtarget: FAIL attach ready pipe\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("eintrtarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    print("eintrtarget: blocking on read forever\n");
    char buffer[4];
    userRead((int)victimFd, buffer, sizeof(buffer));

    /* Only reached if userRead somehow returned on its own -- eintrtest.c
       never writes to the victim pipe, so that would itself be a bug. */
    print("eintrtarget: FAIL read returned without being signaled\n");
    return 1;
}
