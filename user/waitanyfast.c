#include "ulib.h"

/*
 * Only ever spawned by waitanytest.c. Exits immediately with no sleep at
 * all, so waitanytest.c's very first userWaitAny() call is overwhelmingly
 * likely to find this one already Dead (the other child, waitanyslow.c,
 * cannot possibly have finished its own userSleep by then) -- proves
 * threadJoinAnyChild's waitScanFoundDead path picks up an already-dead
 * child without ever needing to block at all.
 */
int main(void)
{
    print("waitanyfast: start, pid=");
    printDec(userGetpid());
    print("\n");
    print("waitanyfast: exiting now\n");
    return 44;
}
