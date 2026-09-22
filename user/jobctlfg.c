#include "ulib.h"

int main(void)
{
    print("jobctlfg: start, pid=");
    printDec(userGetpid());
    print("\n");

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("jobctlfg: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("jobctlfg: FAIL attach victim pipe\n");
        return 1;
    }

    print("jobctlfg: ready, blocking on go pipe\n");
    char buffer[4];
    userRead((int)victimFd, buffer, sizeof(buffer));

    print("jobctlfg: FAIL read returned without being interrupted\n");
    return 1;
}
