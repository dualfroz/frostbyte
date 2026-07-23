#include "ulib.h"

/*
 * Only ever spawned by sigchldtest.c. Does nothing rendezvous-shaped
 * (unlike eintrtarget.c/masktarget.c's own targets) because it does not
 * need to: sigchldtest.c's own proof (a real sigChld raised by the kernel
 * against whoever spawned this process, sched.c's parentId/threadExit)
 * does not depend on any particular ordering between this process's own
 * death and sigchldtest.c noticing it -- sigRaise sets the pending bit on
 * the parent unconditionally, whatever its state is at that instant, and
 * sigchldtest.c's own bounded poll loop (the same "check a flag every
 * userSleep(20)" idiom masktarget.c/nesttarget.c already use) picks it up
 * on its very next natural wake either way. Just runs and exits with a
 * distinctive code sigchldtest.c can verify through userWait after the
 * fact.
 */
int main(void)
{
    print("sigchldchild: start, pid=");
    printDec(userGetpid());
    print("\n");
    print("sigchldchild: exiting now\n");
    return 84;
}
