#include "ulib.h"

int main(int argc, char** argv)
{
    print("eintrtarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 2) {
        print("eintrtarget: FAIL expected one argv (ready handle)\n");
        return 1;
    }

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("eintrtarget: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("eintrtarget: FAIL attach victim pipe\n");
        return 1;
    }

    long readyHandle = parseDec(argv[1]);
    long readyFd = userPipeAttach(readyHandle, 1);
    if (readyFd < 0) {
        print("eintrtarget: FAIL attach ready pipe\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("eintrtarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    print("eintrtarget: blocking on read forever\n");
    char buffer[4];
    userRead((int)victimFd, buffer, sizeof(buffer));

    print("eintrtarget: FAIL read returned without being signaled\n");
    return 1;
}
