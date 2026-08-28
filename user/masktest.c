#include "ulib.h"

/*
 * Boot-time driver for real, ring-3 sa_mask DEFERRAL of signal delivery
 * (signal.h's per-signal sa_mask / Thread.sigHandlerMasks): spawns
 * masktarget.c, and, through three real happens-before rendezvous pipes
 * (never a sleep guess, the same discipline nesttest.c already
 * established), sends it sigUsr1, waits for genuine confirmation that
 * sigUsr1's handler has actually entered and masked sigUsr2, and ONLY THEN
 * sends sigUsr2 -- proving sigUsr2 does NOT interrupt the already-active
 * handler the way nesttest.c's own sigUsr2 deliberately does, but instead
 * waits until sigUsr1's handler genuinely returns before ever running.
 *
 * userSigSend's pending-bit write happens synchronously, inside the syscall
 * itself, before it ever returns to this process -- so by the time it
 * returns 0 below, sigUsr2 is unconditionally pending against masktarget
 * regardless of exactly what masktarget's own thread is doing at that
 * instant. Only the goFd write that follows needs to be strictly ordered
 * after it, which normal program order on THIS process already guarantees
 * -- the same reasoning nesttest.c's own comment already gives for its
 * mirror-image scenario.
 */

int main(void)
{
    print("masktest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long readyHandle = userPipeCreate();
    long handlerReadyHandle = userPipeCreate();
    long goHandle = userPipeCreate();
    if (readyHandle < 0 || handlerReadyHandle < 0 || goHandle < 0) {
        print("masktest: FAIL create pipes\n");
        return 1;
    }

    /* Attach every end THIS process owns before masktarget even exists --
       same "attach first" discipline as jobtest.c/nesttest.c, so none of
       these three slots can be reclaimed by a same-instant close on the
       other side. */
    long readyFd = userPipeAttach(readyHandle, 0);
    long handlerReadyFd = userPipeAttach(handlerReadyHandle, 0);
    long goFd = userPipeAttach(goHandle, 1);
    if (readyFd < 0 || handlerReadyFd < 0 || goFd < 0) {
        print("masktest: FAIL attach pipes\n");
        return 1;
    }

    char readyHandleStr[24];
    char handlerReadyHandleStr[24];
    char goHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    decToString(handlerReadyHandle, handlerReadyHandleStr);
    decToString(goHandle, goHandleStr);

    char* targetArgv[] = { "/bin/masktarget", readyHandleStr,
                            handlerReadyHandleStr, goHandleStr, 0 };
    long target = userSpawn("/bin/masktarget", targetArgv);
    if (target < 0) {
        print("masktest: FAIL spawn masktarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("masktest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("masktest: masktarget ready\n");

    if (userSigSend(target, sigUsr1) != 0) {
        print("masktest: FAIL send sigUsr1 to masktarget\n");
        return 1;
    }
    print("masktest: sent sigUsr1 to masktarget\n");

    char handlerReadyByte;
    /* Real blocking read: does not return until onUsr1 (masktarget.c) has
       actually started running, installed its sa_mask's grip on sigUsr2,
       AND is about to block on goFd -- the happens-before guarantee that
       makes the deferral proof below meaningful instead of a coincidence
       of timing. */
    if (userRead((int)handlerReadyFd, &handlerReadyByte, 1) != 1) {
        print("masktest: FAIL read handler-ready byte\n");
        return 1;
    }
    userClose((int)handlerReadyFd);
    print("masktest: masktarget usr1-handler-entered\n");

    if (userSigSend(target, sigUsr2) != 0) {
        print("masktest: FAIL send sigUsr2 to masktarget\n");
        return 1;
    }
    print("masktest: sent sigUsr2 to masktarget (should stay masked)\n");

    if (userWrite((int)goFd, "g", 1) != 1) {
        print("masktest: FAIL write go byte\n");
        return 1;
    }
    userClose((int)goFd);

    long status = userWait(target);
    print("masktest: masktarget exit status=");
    printDec(status);
    print("\n");
    if (status != 78) {
        print("masktest: FAIL expected masktarget exit status 78\n");
        return 1;
    }
    print("masktest: sa-mask-defer-then-deliver-ok\n");

    return 0;
}
