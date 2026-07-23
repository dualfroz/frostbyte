#include "ulib.h"

/*
 * Only ever spawned by sigtest.c. Installs no handler at all, so the
 * sigTerm sigtest.c sends it must run the OTHER branch of
 * signal.c's sigConsumePending: default disposition, which
 * isr.c's deliverPendingSignals turns into a real threadExit -- proving a
 * signal with no handler actually terminates its target (exit code
 * 128+signum, see isr.c), not just that a handler-based one can be caught.
 * Reaching the FAIL print below would mean the default-terminate path
 * never fired at all.
 */

int main(void)
{
    print("sigdefault: start, pid=");
    printDec(userGetpid());
    print("\n");
    print("sigdefault: waiting to be terminated by default disposition\n");

    for (int i = 0; i < 40; i++)
        userSleep(20);

    print("sigdefault: FAIL still alive after default-disposition signal\n");
    return 1;
}
