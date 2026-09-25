#include "ulib.h"

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

    long child = userSpawn("/bin/alarmdefault", 0);
    if (child < 0) {
        print("alarmtest: FAIL spawn alarmdefault\n");
        return 1;
    }

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
