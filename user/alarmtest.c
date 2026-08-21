#include "ulib.h"

/*
 * Boot-time driver for real alarm(2)/SIGALRM (sched.c's threadSetAlarm,
 * schedule()'s own timer-tick expiry scan, signalnum.h's sigAlrm):
 * schedule() already fires against a per-thread pitTicks() deadline
 * regardless of what that thread is doing, the exact same shape sleepUntil
 * already has for threadSleep -- but expiry here raises a REAL signal
 * (sigRaise) instead of just flipping ThreadState, which means everything
 * this kernel already built for signals (catch, default-terminate,
 * SA_RESTART/EINTR on a retryable syscall, early-wake for a Sleeping/
 * Blocked target) applies to it for free. Three phases, each proving a
 * DIFFERENT piece of that:
 *
 *   phase 1: userAlarm's own "at most one pending alarm" bookkeeping --
 *   scheduling one, then replacing it, then canceling it, checking the
 *   PREVIOUS remaining-seconds return value each time. Purely arithmetic
 *   (see threadSetAlarm's own KTEST coverage in schedTest.c for the same
 *   logic driven directly, deterministically, with no real waiting) --
 *   this phase proves the userspace-visible contract of that same logic,
 *   with no real elapsed time required at all.
 *
 *   phase 2: a REAL 1-second alarm actually firing against a thread
 *   genuinely blocked forever in userRead (the same "attach a victim pipe
 *   no writer will ever attach to" setup eintrtarget.c/restarttarget.c
 *   already use) -- WITH a caught handler, no sigFlagRestart, so the read
 *   must return errInterrupted the instant the handler returns, exactly
 *   like restarttarget.c's own phase 1, just self-triggered by a hardware
 *   timer instead of another process's userSigSend.
 *
 *   phase 3: the SAME real firing, but against a completely separate,
 *   freshly spawned child (alarmdefault.c) that installs NO handler at
 *   all -- SIGALRM's real default disposition is Terminate (signal.c's
 *   sigDefaultIsIgnore deliberately does not list it), so that child must
 *   actually die, with exit code 128+sigAlrm, exactly like sigtest.c's own
 *   sigdefault.c proves for sigTerm.
 *
 * alarmdefault is deliberately SPAWNED before phase 2's own 1-second alarm
 * is even set, not after phase 2 finishes: its own 1-second countdown then
 * runs CONCURRENTLY with this process's phase-2 wait (this kernel's real
 * preemptive scheduler already runs every Runnable/Sleeping thread's own
 * timer independently -- see schedule()'s alarm-expiry scan), so the two
 * real-time waits overlap into roughly one real second of added boot time
 * total, not two sequential ones -- every boot-time demo shares one fixed
 * 25s QEMU budget (tools/ci-test.sh), and stacking real-time waits serially
 * eats into every OTHER demo's own margin against that same clock for no
 * added proof value (phase 2 and phase 3 exercise two independent code
 * paths -- caught+EINTR vs default-Terminate -- neither depends on the
 * other's outcome, so nothing is lost by racing them).
 *
 * If schedule()'s alarm-expiry scan were ever broken (deadline set but
 * never actually checked, or checked but sigRaise never called), phase 2's
 * read call below would simply never return and phase 3's userWait would
 * simply never see its child die -- both hang the whole boot gate to its
 * 25s QEMU timeout rather than merely failing a check, the same
 * fault-injection-visible shape every other "blocked forever until a
 * signal rescues it" demo in this kernel already has.
 */

static volatile int handlerFired = 0;

static void onAlrm(int signum)
{
    (void)signum;
    handlerFired++;
    print("alarmtest: onAlrm fired\n");
}

int main(void)
{
    print("alarmtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    /* Phase 1: pure bookkeeping, no real waiting. */
    unsigned long previous1 = userAlarm(10);
    if (previous1 != 0) {
        print("alarmtest: FAIL expected no prior alarm\n");
        return 1;
    }
    unsigned long previous2 = userAlarm(3);
    if (previous2 != 10) {
        print("alarmtest: FAIL expected 10s remaining on replaced alarm, got ");
        printDec((long)previous2);
        print("\n");
        return 1;
    }
    unsigned long previous3 = userAlarm(0);
    if (previous3 != 3) {
        print("alarmtest: FAIL expected 3s remaining on canceled alarm, got ");
        printDec((long)previous3);
        print("\n");
        return 1;
    }
    unsigned long previous4 = userAlarm(0);
    if (previous4 != 0) {
        print("alarmtest: FAIL expected no alarm pending after cancel, got ");
        printDec((long)previous4);
        print("\n");
        return 1;
    }
    print("alarmtest: alarm-replace-and-cancel-bookkeeping-ok\n");

    /* Spawned here, deliberately BEFORE phase 2 sets its own alarm below --
       see the file comment: this lets alarmdefault's own 1-second alarm
       count down concurrently with phase 2's, instead of only starting
       once phase 2 has already finished. */
    long child = userSpawn("/bin/alarmdefault", 0);
    if (child < 0) {
        print("alarmtest: FAIL spawn alarmdefault\n");
        return 1;
    }

    /* Phase 2: real firing, caught, no sigFlagRestart -- must EINTR. */
    if (userSigAction(sigAlrm, onAlrm, 0, 0) == (long)sigSetFailed) {
        print("alarmtest: FAIL install sigAlrm handler\n");
        return 1;
    }

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("alarmtest: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("alarmtest: FAIL attach victim pipe\n");
        return 1;
    }

    if (userAlarm(1) != 0) {
        print("alarmtest: FAIL expected no prior alarm before phase 2\n");
        return 1;
    }
    print("alarmtest: blocking on read, waiting for real alarm to fire\n");

    char victimBuffer[4];
    long readResult = userRead((int)victimFd, victimBuffer, sizeof(victimBuffer));
    userClose((int)victimFd);

    if (handlerFired != 1) {
        print("alarmtest: FAIL onAlrm handler did not fire exactly once\n");
        return 1;
    }
    if (readResult != errInterrupted) {
        print("alarmtest: FAIL blocking read did not return errInterrupted\n");
        return 1;
    }
    print("alarmtest: alarm-fired-interrupted-blocking-read-ok\n");

    /* Phase 3: real firing, default disposition, must terminate a
       completely separate process -- alarmdefault (spawned above, before
       phase 2) has had its own 1-second alarm counting down this whole
       time, so this wait should return almost immediately, not add a
       second real second of its own. */
    long childStatus = userWait(child);
    print("alarmtest: alarmdefault exit status=");
    printDec(childStatus);
    print("\n");
    if (childStatus != 128 + sigAlrm) {
        print("alarmtest: FAIL expected alarmdefault exit status 128+sigAlrm\n");
        return 1;
    }
    print("alarmtest: alarm-default-disposition-terminated-ok\n");

    print("alarmtest: sigalrm-ok\n");
    return 0;
}
