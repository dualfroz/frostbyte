#include "ulib.h"

/*
 * Only ever spawned by nicetest.c, with no argv at all -- same "*test /
 * *target" shape cwdtest.c/cwdtarget.c already has. Sleeps immediately so
 * nicetest.c gets a real window to renice it from the OUTSIDE while it is
 * genuinely Sleeping, not just Runnable -- proving sysNice (syscall.c)
 * reaches a blocked target, not only one that happens to be running right
 * now. 4096 is an arbitrary but SHARED magic weight, agreed with
 * nicetest.c's own renice call the same way herestest.c/the real shell
 * agree on the literal string "hs-ok" -- no header constant ties the two
 * together, the number is just repeated on both sides.
 */

int main(void)
{
    print("nicetarget: start, pid=");
    printDec(userGetpid());
    print("\n");
    print("nicetarget: ready, sleeping before priority self-check\n");

    userSleep(200);

    long weight = userGetPriority(0);
    print("nicetarget: own weight after sleep=");
    printDec(weight);
    print("\n");
    if (weight != 4096) {
        print("nicetarget: FAIL expected reniced weight 4096\n");
        return 1;
    }
    print("nicetarget: reniced-while-sleeping-ok\n");

    return 0;
}
