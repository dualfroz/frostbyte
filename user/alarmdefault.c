#include "ulib.h"

/*
 * Only ever spawned by alarmtest.c. Installs NO sigAlrm handler at all --
 * the real-Unix default disposition for SIGALRM (signal.c's
 * sigDefaultIsIgnore explicitly does NOT list sigAlrm, so it falls through
 * to the same default-Terminate path every other "real" signal in this
 * kernel already has) is Terminate, not Ignore. userAlarm(1) schedules a
 * real SIGALRM one second from now against THIS process; the read below
 * blocks on a pipe no writer will ever attach to (the same "blocked
 * forever" setup eintrtarget.c/killvictim.c already use), so the only way
 * this process ever leaves that read is the alarm actually firing and
 * killing it -- exactly like eintrtarget.c, except the interrupting signal
 * here comes from schedule()'s own timer-tick expiry scan (sched.c), not
 * from another process's userSigSend. If threadSetAlarm/schedule's expiry
 * scan were ever broken, this process would simply never return from the
 * read below, hanging the whole boot gate to its 25s QEMU timeout instead
 * of merely failing a check -- the same fault-injection-visible shape
 * eintrtest.c's own file comment already establishes for early-wake.
 */

int main(void)
{
    print("alarmdefault: start, pid=");
    printDec(userGetpid());
    print("\n");

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("alarmdefault: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("alarmdefault: FAIL attach victim pipe\n");
        return 1;
    }

    if (userAlarm(1) != 0) {
        print("alarmdefault: FAIL unexpected pre-existing alarm\n");
        return 1;
    }

    print("alarmdefault: blocking on read forever\n");
    char buffer[4];
    userRead((int)victimFd, buffer, sizeof(buffer));

    /* Only reached if userRead somehow returned on its own -- nothing ever
       writes to the victim pipe, so the alarm not firing (or firing but
       not actually terminating this process) is the only way to get here. */
    print("alarmdefault: FAIL read returned without being signaled\n");
    return 1;
}
