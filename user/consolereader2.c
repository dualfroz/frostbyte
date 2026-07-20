#include "ulib.h"

/* See consolereader1.c's comment -- an entirely independent process reading
   the same fd 0, spawned separately by kmain.c, sharing nothing with
   consolereader1 but the fact that both happen to be alive when
   consoleInjectorThread's single synthetic keystroke fires. If fan-out
   ever regressed back to one shared buffer, whichever of the two processes
   the scheduler happened to run first would steal the only queued byte and
   the other would hang forever -- the boot gate's timeout would catch that
   as a missing needle rather than a hang, since only this process is
   missing, not the whole machine. */

int main(void)
{
    print("consolereader2: start, pid=");
    printDec(userGetpid());
    print(", blocking on fd 0\n");

    char c = 0;
    long got = userRead(0, &c, 1);
    if (got != 1) {
        print("consolereader2: FAIL read\n");
        return 1;
    }

    print("consolereader2: got byte '");
    userWrite(1, &c, 1);
    print("' -> console-fanout-2-ok\n");
    return 0;
}
