#include "ulib.h"

int main(void)
{
    print("sigchldchild: start, pid=");
    printDec(userGetpid());
    print("\n");
    print("sigchldchild: exiting now\n");
    return 84;
}
