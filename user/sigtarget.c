#include "ulib.h"

/*
 * Only ever spawned by sigtest.c, with a shared pipe handle as argv[1]
 * (decToString'd, exactly like fifomaster.c's children -- see that file).
 * Proves real, catchable signal delivery end to end: install a handler for
 * sigUsr1 through userSigAction, wait to actually be interrupted by one
 * arriving from a completely separate process (sigtest.c, via
 * userSigSend -- not the kernel injecting it directly), and keep running
 * normally afterwards -- the handler firing AND the loop below correctly
 * resuming (not corrupting registers, not re-entering main, not looping
 * forever) is the round trip through signal.c's sigConsumePending, the
 * shared trampoline page (sigTrampoline.S), and sysSigReturn all actually
 * working together, not just each piece compiling.
 *
 * The pipe exists purely to close a real race sigtest.c originally papered
 * over with a fixed userSleep: sigUsr1's default disposition (no handler
 * installed) is Terminate, not Ignore, so a signal that arrives before
 * userSigAction below has actually run would kill this process instead of
 * being caught -- observed for real under host load (sigtarget killed by
 * "uncaught signal 10" before it ever printed "waiting for signal").
 * Writing one byte AFTER installing the handler, and having sigtest.c
 * block on reading it before ever calling userSigSend, makes "handler
 * installed" happen-before "signal sent" for real, the same guarantee
 * fifomaster.c's children get from their own shared pipe.
 */

static volatile int handlerFired = 0;
static volatile int handlerSignum = 0;

static void onUsr1(int signum)
{
    handlerFired = 1;
    handlerSignum = signum;
    print("sigtarget: handler invoked signum=");
    printDec(signum);
    print("\n");
}

int main(int argc, char** argv)
{
    print("sigtarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 2) {
        print("sigtarget: FAIL expected one argv (pipe handle)\n");
        return 1;
    }

    if (userSigAction(sigUsr1, onUsr1, 0, 0) == (long)sigSetFailed) {
        print("sigtarget: FAIL install handler\n");
        return 1;
    }

    long handle = parseDec(argv[1]);
    long readyFd = userPipeAttach(handle, 1);
    if (readyFd < 0) {
        print("sigtarget: FAIL attach ready pipe\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("sigtarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    print("sigtarget: waiting for signal\n");

    /* Several short sleeps rather than one long one: this kernel only
       checks for a pending signal when the target thread actually returns
       to ring 3 (isr.c's deliverPendingSignals) -- a thread that is still
       genuinely Sleeping is not scheduled at all, so a shorter sleep gives
       sigtest.c's userSigSend more chances to land promptly, without
       depending on exact timing between the two independent processes. */
    for (int i = 0; i < 40 && !handlerFired; i++)
        userSleep(20);

    if (!handlerFired) {
        print("sigtarget: FAIL handler never fired\n");
        return 1;
    }
    if (handlerSignum != sigUsr1) {
        print("sigtarget: FAIL wrong signum delivered\n");
        return 1;
    }
    print("sigtarget: handler-fired-ok\n");

    /* Reached only if sysSigReturn correctly restored the exact
       pre-signal context: a corrupted resume (wrong rip/rsp/registers)
       would have crashed, hung, or skipped straight past this line
       instead of returning here from the interrupted sleep loop above. */
    print("sigtarget: resumed-after-handler-ok\n");
    return 42;
}
