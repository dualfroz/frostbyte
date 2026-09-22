#include "ulib.h"

int main(void)
{
    print("exectarget: running, pid=");
    printDec(userGetpid());
    print("\n");
    return 7;
}
