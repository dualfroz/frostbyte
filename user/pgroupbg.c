#include "ulib.h"

/*
 * Spawned directly by kmain.c at boot, alongside pgroupfg.c. Deliberately
 * NEVER told to keyboardSetForegroundPgid -- every boot-time spawnUser call
 * becomes its own new process group leader by default (sched.c's
 * initialPgid), so this process's pgid already differs from pgroupfg's
 * without any extra call, the same way two independently-launched
 * background jobs in a real shell would each get their own group. This is
 * the CONTROL side of the pgroupfg/pgroupbg pair: pgroupfg proves the
 * injected Ctrl-C reaches the foreground group and kills it; this file
 * proves that exact same event does NOT reach a DIFFERENT group at all --
 * if keyboard.c's scoping were ever broken to deliver sigInt/sigTstp to
 * every live thread (the bug this whole subsystem exists to prevent), this
 * process would already be Dead by the time kmain.c's injector later tries
 * to prove it alive, and every needle below would simply never appear.
 *
 * Along the way, exercises the actual syscall boundary (sysSetpgid/
 * sysGetpgid, syscall.c) that sched.c's own KTEST coverage cannot reach on
 * its own (ktestRunAll runs before schedStart(), with no int 0x80 path at
 * all -- see schedTest.c's file comment): userGetpgid(0) must already
 * equal this process's own pid (new-group-leader default), and an
 * explicit, real setpgid(0, 0) self-affirmation (the same defensive call a
 * real shell places a background job's own PID with, even when it is
 * already its own leader) must be a harmless no-op.
 */

static volatile int handlerFired = 0;

static void onUsr1(int signum)
{
    (void)signum;
    handlerFired = 1;
    print("pgroupbg: still-alive-after-ctrlc-ok\n");
}

int main(void)
{
    long myPid = userGetpid();
    print("pgroupbg: start, pid=");
    printDec(myPid);
    print("\n");

    long pgid = userGetpgid(0);
    if (pgid != myPid) {
        print("pgroupbg: FAIL expected to be its own group leader\n");
        return 1;
    }
    if (userSetpgid(0, 0) != 0) {
        print("pgroupbg: FAIL self setpgid(0,0) rejected\n");
        return 1;
    }
    if (userGetpgid(0) != myPid) {
        print("pgroupbg: FAIL pgid changed after self-affirming setpgid\n");
        return 1;
    }
    print("pgroupbg: own-process-group-ok\n");

    if (userSigAction(sigUsr1, onUsr1, 0, 0) == (long)sigSetFailed) {
        print("pgroupbg: FAIL install handler\n");
        return 1;
    }

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("pgroupbg: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("pgroupbg: FAIL attach victim pipe\n");
        return 1;
    }

    print("pgroupbg: ready, blocking on go pipe\n");
    char buffer[4];
    userRead((int)victimFd, buffer, sizeof(buffer));

    if (!handlerFired) {
        print("pgroupbg: FAIL read returned without being signaled\n");
        return 1;
    }

    /* Reached only if the read was genuinely interrupted (errInterrupted,
       no sigFlagRestart) by onUsr1 above, itself only ever sent by
       kmain.c's own injector thread well AFTER the Ctrl-C event -- proving
       this process was never touched by it at all. */
    print("pgroupbg: resumed-after-signal-ok\n");
    return 0;
}
