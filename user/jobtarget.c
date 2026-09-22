#include "ulib.h"

int main(int argc, char** argv)
{
    print("jobtarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 3) {
        print("jobtarget: FAIL expected two argv (ready handle, go handle)\n");
        return 1;
    }

    long readyHandle = parseDec(argv[1]);
    long goHandle = parseDec(argv[2]);

    long goFd = userPipeAttach(goHandle, 0);
    if (goFd < 0) {
        print("jobtarget: FAIL attach go pipe as reader\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 1);
    if (readyFd < 0) {
        print("jobtarget: FAIL attach ready pipe as writer\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("jobtarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("jobtarget: ready, blocking on go pipe\n");

    char goByte = 0;
    if (userRead((int)goFd, &goByte, 1) != 1) {
        print("jobtarget: FAIL read go byte\n");
        return 1;
    }
    userClose((int)goFd);

    if (goByte != 'g') {
        print("jobtarget: FAIL wrong go byte\n");
        return 1;
    }
    print("jobtarget: received go\n");

    return 55;
}
