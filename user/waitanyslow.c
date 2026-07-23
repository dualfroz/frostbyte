#include "ulib.h"

/*
 * Only ever spawned by waitanytest.c. Sleeps a real, non-trivial amount
 * before exiting -- long enough that waitanytest.c's own userWaitAny()
 * calls cannot possibly race past it by luck: its first call happens
 * essentially immediately after spawning both children (see
 * waitanytest.c's own file comment), so this thread genuinely cannot be
 * Dead yet by the time that first call runs. That is what forces
 * waitanytest.c's SECOND userWaitAny() call to take threadJoinAnyChild's
 * waitScanNoneDead branch and actually block through the real
 * retry-syscall path, rather than getting lucky and finding it already
 * dead too.
 */
int main(void)
{
    print("waitanyslow: start, pid=");
    printDec(userGetpid());
    print("\n");
    userSleep(150);
    print("waitanyslow: exiting now\n");
    return 33;
}
