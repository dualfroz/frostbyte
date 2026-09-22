#include "ulib.h"

int main(void)
{
    print("consolereader1: start, pid=");
    printDec(userGetpid());
    print(", blocking on fd 0\n");

    char c = 0;
    long got = userRead(0, &c, 1);
    if (got != 1) {
        print("consolereader1: FAIL read\n");
        return 1;
    }

    print("consolereader1: got byte '");
    userWrite(1, &c, 1);
    print("' -> console-fanout-1-ok\n");
    return 0;
}
