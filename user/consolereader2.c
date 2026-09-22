#include "ulib.h"

int main(void)
{
    print("consolereader2: start, pid=");
    printDec(userGetpid());
    print(", blocking on fd 0\n");

    char c = 0;
    long got = userRead(0, &c, 1);
    if (got != 1) {
        print("consolereader2: FAIL read\n");
        return 1;
    }

    print("consolereader2: got byte '");
    userWrite(1, &c, 1);
    print("' -> console-fanout-2-ok\n");
    return 0;
}
