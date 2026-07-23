#include "ulib.h"

/* A plain child program: only ever launched via proctest.c's userSpawn, so
   its pid is a genuinely different, freshly created thread rather than the
   caller's own (unlike exectarget.c, which replaces the caller in place). */

int main(void)
{
    print("spawntarget: child running, pid=");
    printDec(userGetpid());
    print("\n");
    return 3;
}
