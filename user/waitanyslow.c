#include "ulib.h"

int main(void)
{
    print("waitanyslow: start, pid=");
    printDec(userGetpid());
    print("\n");
    userSleep(150);
    print("waitanyslow: exiting now\n");
    return 33;
}
