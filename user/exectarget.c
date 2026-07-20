#include "ulib.h"

/*
 * Never spawned directly by kmain.c or by proctest.c's userSpawn -- the
 * only way this binary's "exectarget: running" line can ever appear in the
 * boot log is if proctest.c's userExec("/bin/exectarget") call actually
 * replaced proctest's own image with this one (see proctest.c). Its pid is
 * therefore the SAME pid proctest was given at spawnUser time in kmain.c,
 * which is what kmain.c's execReaperThread checks via threadJoin to prove
 * exec() preserves process identity, not just that some program ran.
 */

int main(void)
{
    print("exectarget: running, pid=");
    printDec(userGetpid());
    print("\n");
    return 7;
}
