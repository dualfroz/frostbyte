#include "ulib.h"

int main(int argc, char** argv)
{
    print("selfstoptarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 2) {
        print("selfstoptarget: FAIL expected one argv (proof pipe handle)\n");
        return 1;
    }

    long handle = parseDec(argv[1]);
    long proofFd = userPipeAttach(handle, 1);
    if (proofFd < 0) {
        print("selfstoptarget: FAIL attach proof pipe as writer\n");
        return 1;
    }

    print("selfstoptarget: self-stopping now\n");

    if (userSigSend(userGetpid(), sigStop) != 0) {
        print("selfstoptarget: FAIL self-directed sigStop rejected\n");
        return 1;
    }

    print("selfstoptarget: resumed-after-self-stop-ok\n");

    if (userWrite((int)proofFd, "p", 1) != 1) {
        print("selfstoptarget: FAIL write proof byte\n");
        return 1;
    }
    userClose((int)proofFd);

    return 66;
}
