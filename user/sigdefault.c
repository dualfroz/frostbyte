#include "ulib.h"

int main(int argc, char** argv)
{
    print("sigdefault: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 2) {
        print("sigdefault: FAIL expected one argv (ready handle)\n");
        return 1;
    }

    long readyFd = userPipeAttach(parseDec(argv[1]), 1);
    if (readyFd < 0) {
        print("sigdefault: FAIL attach ready pipe\n");
        return 1;
    }

    print("sigdefault: waiting to be terminated by default disposition\n");
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("sigdefault: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    for (int i = 0; i < 40; i++)
        userSleep(20);

    print("sigdefault: FAIL still alive after default-disposition signal\n");
    return 1;
}
