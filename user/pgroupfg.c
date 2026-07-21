#include "ulib.h"

/*
 * Spawned directly by kmain.c at boot, alongside pgroupbg.c -- the live
 * ring-3 proof that sched.h's new Thread.pgid and keyboard.c's new real
 * Ctrl-C actually scope a terminal-generated signal to the FOREGROUND
 * process group, not to "every process reading a keyboard" or "one single
 * hardcoded target". kmain.c makes this process's own pgid (its default,
 * unchanged: every boot-time spawnUser call becomes its own new group
 * leader, sched.c's initialPgid) the foreground via
 * keyboardSetForegroundPgid right after spawning it.
 *
 * Installs NO handler for sigInt at all -- the real-world default
 * disposition (signal.c's sigDefaultIsIgnore does not list sigInt) is
 * Terminate, exactly like a real un-caught Ctrl-C killing a foreground
 * program with no signal handling of its own. The read below blocks on a
 * pipe no writer will ever attach to (the same "blocked forever" setup
 * eintrtarget.c/killvictim.c/alarmdefault.c already use), so the only way
 * this process ever leaves it is the kernel-injected Ctrl-C's early-wake
 * (signal.c's sigWouldDeliver/sched.c's threadWakeForSignal, the same
 * mechanism eintrtarget.c's own sigTerm already proves) actually reaching
 * a genuinely Blocked thread and terminating it. If keyboard.c's group
 * scoping were ever broken such that this process were somehow excluded
 * from its own foreground group, this process would simply never return
 * from the read below, hanging the whole boot gate to its 25s QEMU timeout
 * instead of merely failing a check -- the same fault-injection-visible
 * shape eintrtest.c's own file comment already establishes.
 */

int main(void)
{
    print("pgroupfg: start, pid=");
    printDec(userGetpid());
    print("\n");

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("pgroupfg: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("pgroupfg: FAIL attach victim pipe\n");
        return 1;
    }

    print("pgroupfg: ready, blocking on go pipe\n");
    char buffer[4];
    userRead((int)victimFd, buffer, sizeof(buffer));

    /* Only reached if userRead somehow returned on its own -- nothing ever
       writes to the victim pipe, so the foreground Ctrl-C never arriving
       (or arriving but not actually terminating this process) is the only
       way to get here. */
    print("pgroupfg: FAIL read returned without being interrupted\n");
    return 1;
}
