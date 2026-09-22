#include "ulib.h"

int main(void)
{
    print("waitanyfast: start, pid=");
    printDec(userGetpid());
    print("\n");
    print("waitanyfast: exiting now\n");
    return 44;
}
