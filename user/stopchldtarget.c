#include "ulib.h"

int main(int argc, char** argv)
{
    print("stopchldtarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 3) {
        print("stopchldtarget: FAIL expected two argv (ready handle, go handle)\n");
        return 1;
    }

    long readyHandle = parseDec(argv[1]);
    long goHandle = parseDec(argv[2]);

    long goFd = userPipeAttach(goHandle, 0);
    if (goFd < 0) {
        print("stopchldtarget: FAIL attach go pipe as reader\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 1);
    if (readyFd < 0) {
        print("stopchldtarget: FAIL attach ready pipe as writer\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("stopchldtarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("stopchldtarget: ready, blocking on go pipe\n");

    char goByte = 0;
    if (userRead((int)goFd, &goByte, 1) != 1) {
        print("stopchldtarget: FAIL read go byte\n");
        return 1;
    }
    userClose((int)goFd);

    if (goByte != 'g') {
        print("stopchldtarget: FAIL wrong go byte\n");
        return 1;
    }
    print("stopchldtarget: received go\n");

    return 66;
}
