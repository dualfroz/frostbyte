#include "ulib.h"

int main(int argc, char** argv)
{
    print("killvictim: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 2) {
        print("killvictim: FAIL expected one argv (ready handle)\n");
        return 1;
    }

    long handle = userPipeCreate();
    if (handle < 0) {
        print("killvictim: FAIL pipe create\n");
        return 1;
    }

    long fd = userPipeAttach(handle, 0);
    if (fd < 0) {
        print("killvictim: FAIL attach\n");
        return 1;
    }

    long readyFd = userPipeAttach(parseDec(argv[1]), 1);
    if (readyFd < 0) {
        print("killvictim: FAIL attach ready pipe\n");
        return 1;
    }

    print("killvictim: blocking on read forever\n");
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("killvictim: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    char buffer[4];
    userRead((int)fd, buffer, sizeof(buffer));

    print("killvictim: FAIL read returned without being killed\n");
    return 1;
}
