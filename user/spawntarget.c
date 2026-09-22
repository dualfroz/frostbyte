#include "ulib.h"

int main(void)
{
    print("spawntarget: child running, pid=");
    printDec(userGetpid());
    print("\n");
    return 3;
}
