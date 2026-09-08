#include "ulib.h"

/*
 * Boot-time driver for the whole signal-delivery demo (signal.h). Spawns
 * two independent children -- sigtarget.c (installs a real handler) and
 * sigdefault.c (installs none) -- and sends each a real signal through the
 * full syscall path (userSigSend, not a kernel-internal shortcut), proving
 * both outcomes sigConsumePending can produce: caught-and-resumed, and
 * uncaught-and-terminated.
 *
 * sigtarget needs a real happens-before guarantee, not a sleep guess:
 * sigUsr1's default disposition is Terminate, so a signal that arrives
 * before sigtarget's userSigAction has actually run would kill it instead
 * of being caught -- this was originally "give it 100ms and hope", which
 * really did fail under host load (sigtarget killed by the uncaught
 * default before it ever installed its handler). See sigtarget.c for the
 * shared-pipe rendezvous (fifomaster.c's pattern) that fixes it: this
 * process blocks on a real read for sigtarget's own "handler installed"
 * byte before ever calling userSigSend.
 *
 * sigdefault has no such window -- its default disposition applies from
 * the moment it exists, whether or not it has run any code yet -- so a
 * plain sleep-then-send (mirroring killtest.c's own "give the victim time
 * to reach its blocking read") is genuinely sufficient there.
 */

int main(void)
{
    print("sigtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userSigSend(999999, sigUsr1) != -1) {
        print("sigtest: FAIL signal to bogus pid should fail\n");
        return 1;
    }
    print("sigtest: signal to bad pid rejected\n");

    if (userSigAction(sigKill, (SigHandler)0, 0, 0) != (long)sigSetFailed) {
        print("sigtest: FAIL installing a sigKill handler should be rejected\n");
        return 1;
    }
    print("sigtest: sigkill-handler-rejected-ok\n");

    long readyHandle = userPipeCreate();
    if (readyHandle < 0) {
        print("sigtest: FAIL create ready pipe\n");
        return 1;
    }

    /* Attach as reader BEFORE sigtarget even exists, let alone before it
       can possibly attach+write+close its own end: pipeCloseWrite (pipe.c)
       reclaims a detached pipe's slot the moment readers AND writers both
       read 0, which they still would right after sigtarget's write+close
       if this process had not attached yet -- observed for real (sigtarget
       raced ahead, wrote its ready byte, closed, and freed the slot before
       this process's own attach below ever ran, which then failed outright
       on the now-invalid handle). Attaching first makes readers==1 from
       the very start, so that race window never opens. */
    long readyFd = userPipeAttach(readyHandle, 0);
    if (readyFd < 0) {
        print("sigtest: FAIL attach ready pipe\n");
        return 1;
    }

    char readyHandleStr[24];
    decToString(readyHandle, readyHandleStr);

    char* targetArgv[] = { "/bin/sigtarget", readyHandleStr, 0 };
    long target = userSpawn("/bin/sigtarget", targetArgv);
    if (target < 0) {
        print("sigtest: FAIL spawn sigtarget\n");
        return 1;
    }

    char readyByte;
    /* Real blocking read: does not return until sigtarget has actually
       installed its handler and written this byte (see sigtarget.c) --
       the happens-before guarantee a fixed userSleep could not give. */
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("sigtest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    if (userSigSend(target, sigUsr1) != 0) {
        print("sigtest: FAIL send sigUsr1 to sigtarget\n");
        return 1;
    }
    print("sigtest: sent sigUsr1 to sigtarget\n");

    long status = userWait(target);
    print("sigtest: sigtarget exit status=");
    printDec(status);
    print("\n");
    if (status != 42) {
        print("sigtest: FAIL expected sigtarget exit status 42\n");
        return 1;
    }
    print("sigtest: handler-roundtrip-ok\n");

    long defaultTarget = userSpawn("/bin/sigdefault", 0);
    if (defaultTarget < 0) {
        print("sigtest: FAIL spawn sigdefault\n");
        return 1;
    }

    userSleep(100);

    if (userSigSend(defaultTarget, sigTerm) != 0) {
        print("sigtest: FAIL send sigTerm to sigdefault\n");
        return 1;
    }
    print("sigtest: sent sigTerm to sigdefault\n");

    long defaultStatus = userWait(defaultTarget);
    print("sigtest: sigdefault exit status=");
    printDec(defaultStatus);
    print("\n");
    if (defaultStatus != 128 + sigTerm) {
        print("sigtest: FAIL expected sigdefault exit status 128+sigTerm\n");
        return 1;
    }
    print("sigtest: default-terminate-ok\n");

    return 0;
}
