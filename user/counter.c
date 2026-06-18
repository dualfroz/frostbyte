#include "ulib.h"

int main(void)
{
    long pid = userGetpid();

    for (int step = 0; step < 5; step++) {
        print("counter: pid=");
        printDec(pid);
        print(" step=");
        printDec(step);
        print("\n");
        userSleep(30);
    }

    print("counter: finished counting\n");
    return 0;
}
