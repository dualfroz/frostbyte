#include "ulib.h"

/*
 * Boot-time driver for real, ring-3 NESTED signal delivery (signal.h's
 * per-signal sa_mask / Thread.sigHandlerDepth stack): spawns nesttarget.c,
 * and, through three real happens-before rendezvous pipes (never a sleep
 * guess), sends it sigUsr1, waits for genuine confirmation that sigUsr1's
 * handler has actually entered and blocked, and ONLY THEN sends sigUsr2 --
 * proving sigUsr2 interrupts an ALREADY-ACTIVE handler with a second one of
 * its own, rather than either being dropped, deferred until sigUsr1 returns
 * (which is what an empty-but-still-"one handler at a time" gate, the
 * architecture this replaced, would have done), or racing ahead of sigUsr1
 * even starting.
 *
 * userSigSend's pending-bit write happens synchronously, inside the syscall
 * itself, before it ever returns to this process -- so by the time it
 * returns 0 below, sigUsr2 is unconditionally pending against nesttarget
 * regardless of exactly what nesttarget's own thread is doing at that
 * instant. Only the goFd write that follows needs to be strictly ordered
 * after it, which normal program order on THIS process already guarantees.
 */

int main(void)
{
    print("nesttest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long readyHandle = userPipeCreate();
    long handlerReadyHandle = userPipeCreate();
    long goHandle = userPipeCreate();
    if (readyHandle < 0 || handlerReadyHandle < 0 || goHandle < 0) {
        print("nesttest: FAIL create pipes\n");
        return 1;
    }

    /* Attach every end THIS process owns before nesttarget even exists --
       same "attach first" discipline as jobtest.c, so none of these three
       slots can be reclaimed by a same-instant close on the other side. */
    long readyFd = userPipeAttach(readyHandle, 0);
    long handlerReadyFd = userPipeAttach(handlerReadyHandle, 0);
    long goFd = userPipeAttach(goHandle, 1);
    if (readyFd < 0 || handlerReadyFd < 0 || goFd < 0) {
        print("nesttest: FAIL attach pipes\n");
        return 1;
    }

    char readyHandleStr[24];
    char handlerReadyHandleStr[24];
    char goHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    decToString(handlerReadyHandle, handlerReadyHandleStr);
    decToString(goHandle, goHandleStr);

    char* targetArgv[] = { "/bin/nesttarget", readyHandleStr,
                            handlerReadyHandleStr, goHandleStr, 0 };
    long target = userSpawn("/bin/nesttarget", targetArgv);
    if (target < 0) {
        print("nesttest: FAIL spawn nesttarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("nesttest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("nesttest: nesttarget ready\n");

    if (userSigSend(target, sigUsr1) != 0) {
        print("nesttest: FAIL send sigUsr1 to nesttarget\n");
        return 1;
    }
    print("nesttest: sent sigUsr1 to nesttarget\n");

    char handlerReadyByte;
    /* Real blocking read: does not return until onUsr1 (nesttarget.c) has
       actually started running AND is about to block on goFd -- the
       happens-before guarantee that makes the nested-delivery proof below
       meaningful instead of a coincidence of timing. */
    if (userRead((int)handlerReadyFd, &handlerReadyByte, 1) != 1) {
        print("nesttest: FAIL read handler-ready byte\n");
        return 1;
    }
    userClose((int)handlerReadyFd);
    print("nesttest: nesttarget usr1-handler-entered\n");

    if (userSigSend(target, sigUsr2) != 0) {
        print("nesttest: FAIL send sigUsr2 to nesttarget\n");
        return 1;
    }
    print("nesttest: sent sigUsr2 to nesttarget\n");

    if (userWrite((int)goFd, "g", 1) != 1) {
        print("nesttest: FAIL write go byte\n");
        return 1;
    }
    userClose((int)goFd);

    long status = userWait(target);
    print("nesttest: nesttarget exit status=");
    printDec(status);
    print("\n");
    if (status != 77) {
        print("nesttest: FAIL expected nesttarget exit status 77\n");
        return 1;
    }
    print("nesttest: nested-signal-delivery-ok\n");

    return 0;
}
