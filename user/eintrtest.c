#include "ulib.h"

/*
 * Boot-time driver for early-wake signal delivery (signal.h/sched.c's
 * threadWakeForSignal): spawns eintrtarget.c, waits for a real
 * happens-before confirmation that it is about to block forever on a read
 * that no writer will ever satisfy (the same unsatisfiable-by-construction
 * setup killtest.c's killvictim.c already established for threadKill), and
 * sends it a real sigTerm through userSigSend -- an ORDINARY signal with
 * its real default disposition (Terminate), not threadKill's direct
 * bypass.
 *
 * If userWait below ever hung the whole boot (see ci-test.sh's 25s QEMU
 * timeout), that would itself BE the regression this proves against: a
 * Blocked thread that never naturally wakes has no other way back to ring 3
 * for isr.c's deliverPendingSignals to ever reach it, without the
 * early-wake this test exists to exercise.
 */

int main(void)
{
    print("eintrtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long readyHandle = userPipeCreate();
    if (readyHandle < 0) {
        print("eintrtest: FAIL create ready pipe\n");
        return 1;
    }
    long readyFd = userPipeAttach(readyHandle, 0);
    if (readyFd < 0) {
        print("eintrtest: FAIL attach ready pipe\n");
        return 1;
    }

    char readyHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    char* targetArgv[] = { "/bin/eintrtarget", readyHandleStr, 0 };
    long target = userSpawn("/bin/eintrtarget", targetArgv);
    if (target < 0) {
        print("eintrtest: FAIL spawn eintrtarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("eintrtest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("eintrtest: eintrtarget ready\n");

    if (userSigSend(target, sigTerm) != 0) {
        print("eintrtest: FAIL send sigTerm to eintrtarget\n");
        return 1;
    }
    print("eintrtest: sent sigTerm to eintrtarget\n");

    long status = userWait(target);
    print("eintrtest: eintrtarget exit status=");
    printDec(status);
    print("\n");
    if (status != 128 + sigTerm) {
        print("eintrtest: FAIL expected eintrtarget exit status 128+sigTerm\n");
        return 1;
    }
    print("eintrtest: early-wake-signal-ok\n");

    return 0;
}
