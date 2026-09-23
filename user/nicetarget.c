#include "ulib.h"

int main(int argc, char** argv)
{
    print("nicetarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 3) {
        print("nicetarget: FAIL expected two argv (ready handle, go handle)\n");
        return 1;
    }

    long goFd = userPipeAttach(parseDec(argv[2]), 0);
    if (goFd < 0) {
        print("nicetarget: FAIL attach go pipe\n");
        return 1;
    }
    long readyFd = userPipeAttach(parseDec(argv[1]), 1);
    if (readyFd < 0) {
        print("nicetarget: FAIL attach ready pipe\n");
        return 1;
    }

    print("nicetarget: ready, blocking on go pipe\n");
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("nicetarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    char goByte;
    if (userRead((int)goFd, &goByte, 1) != 1) {
        print("nicetarget: FAIL read go byte\n");
        return 1;
    }
    userClose((int)goFd);

    long weight = userGetPriority(0);
    print("nicetarget: own weight after wake=");
    printDec(weight);
    print("\n");
    if (weight != 4096) {
        print("nicetarget: FAIL expected reniced weight 4096\n");
        return 1;
    }
    print("nicetarget: reniced-while-sleeping-ok\n");

    return 0;
}
