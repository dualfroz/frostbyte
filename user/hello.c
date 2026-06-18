#include "ulib.h"

int main(void)
{
    print("hello: greetings from ring 3, pid=");
    printDec(userGetpid());
    print("\n");

    userYield();

    print("hello: resumed after yield, exiting cleanly\n");
    return 7;
}
