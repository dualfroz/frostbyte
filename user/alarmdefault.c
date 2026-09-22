#include "ulib.h"

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

    print("alarmdefault: FAIL read returned without being signaled\n");
    return 1;
}
