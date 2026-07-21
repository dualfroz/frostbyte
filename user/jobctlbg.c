#include "ulib.h"

/*
 * Companion to jobctlfg.c: loaded the same way, on demand, when kmain.c's
 * jobctlDriverThread types "jobctlbg &" at the real interactive shell.
 * The trailing "&" (shell.c's runProgram) routes this through
 * jobctlRunBackground (jobctl.c) instead of jobctlSpawnForeground -- own
 * new process group leader exactly like jobctlfg.c, but the shell never
 * calls keyboardSetForegroundPgid for it and never blocks waiting for it
 * either, a real shell's `&` job semantics. This is the CONTROL side of
 * the jobctlfg/jobctlbg pair: jobctlfg proves the shell's foreground
 * grant actually reaches an injected Ctrl-C; this file proves that exact
 * same event does NOT reach a job the shell deliberately left in the
 * background -- if shell.c's `&` handling or jobctl.c's group-leader
 * assignment were ever broken such that this process shared jobctlfg's
 * group (or somehow ended up foreground itself), this process would
 * already be Dead by the time jobctlDriverThread later tries to prove it
 * alive, and its own needles below would simply never appear.
 *
 * Also exercises the actual sysGetpgid syscall boundary (unreachable from
 * KTEST, which runs before schedStart() with no int 0x80 path at all) to
 * confirm jobctl.c's threadSetPgid(pid, pid) call genuinely took effect
 * from ring3's own point of view, not just internally.
 */

static volatile int handlerFired = 0;

static void onUsr1(int signum)
{
    (void)signum;
    handlerFired = 1;
    print("jobctlbg: still-alive-after-ctrlc-ok\n");
}

int main(void)
{
    long myPid = userGetpid();
    print("jobctlbg: start, pid=");
    printDec(myPid);
    print("\n");

    long pgid = userGetpgid(0);
    if (pgid != myPid) {
        print("jobctlbg: FAIL expected to be its own group leader\n");
        return 1;
    }
    print("jobctlbg: own-process-group-ok\n");

    if (userSigAction(sigUsr1, onUsr1, 0, 0) == (long)sigSetFailed) {
        print("jobctlbg: FAIL install handler\n");
        return 1;
    }

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("jobctlbg: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("jobctlbg: FAIL attach victim pipe\n");
        return 1;
    }

    print("jobctlbg: ready, blocking on go pipe\n");
    char buffer[4];
    userRead((int)victimFd, buffer, sizeof(buffer));

    if (!handlerFired) {
        print("jobctlbg: FAIL read returned without being signaled\n");
        return 1;
    }

    /* Reached only if the read was genuinely interrupted (errInterrupted,
       no sigFlagRestart) by onUsr1 above, sent by kmain.c's own driver
       thread well AFTER the Ctrl-C injected for jobctlfg -- proving this
       backgrounded process was never touched by it. */
    print("jobctlbg: resumed-after-signal-ok\n");
    return 0;
}
