#include "ulib.h"

/*
 * Exercises the process-management syscalls added this wave: sysSpawn
 * (posix_spawn()-lite -- loads a binary into a brand new thread) and
 * sysWait (threadJoin, sched.c/sched.h) together, then sysExec (execve()-
 * lite -- replaces this very process's image in place, see syscall.c
 * doExec). Split across two helper binaries deliberately: spawntarget.c is
 * only ever spawned (never exec'd), exectarget.c is only ever exec'd
 * (never spawned) -- frostbyte has one shared address space system-wide
 * (paging.c has a single page table, no per-process CR3), so two binaries
 * whose fixed link addresses collide can never be genuinely live at the
 * same time. Keeping "the spawn target" and "the exec target" as distinct
 * binaries, and waiting for the spawned child before ever calling exec,
 * means this test never asks the kernel to do something the architecture
 * cannot support.
 *
 * This function never returns on its own success path: the final call is
 * userExec("/bin/exectarget"), which -- if the syscall does its job --
 * replaces this thread's own code/stack/pid identity with exectarget's, so
 * "proctest" never gets to print anything past that call. kmain.c's
 * execReaperThread separately threadJoin()s this same pid and checks it
 * eventually reports exectarget's exit code, which is the only way to
 * observe that the exec() actually happened from outside this process.
 */

int main(void)
{
    print("proctest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long child = userSpawn("/bin/spawntarget", 0);
    if (child < 0) {
        print("proctest: FAIL spawn /bin/spawntarget\n");
        return 1;
    }

    long status = userWait(child);
    if (status != 3) {
        print("proctest: FAIL wait returned unexpected status\n");
        return 1;
    }
    print("proctest: spawn+wait ok\n");

    if (userSpawn("/bin/does-not-exist", 0) != -1) {
        print("proctest: FAIL spawn of missing binary should fail\n");
        return 1;
    }
    print("proctest: spawn bad path rejected\n");

    if (userWait(999999) != -1) {
        print("proctest: FAIL wait on bogus pid should fail immediately\n");
        return 1;
    }
    print("proctest: wait bad pid rejected\n");

    /* argtest.c checks its own argc/argv against exactly this list --
       proves the whole chain (copyUserArgv copying these out of THIS
       process's memory, elfBuildInitialStack laying them onto the child's
       brand new stack, crt.S's _start reading them back off of it) rather
       than just that spawn/wait still works with a null argv. */
    char* argtestArgv[] = { "/bin/argtest", "alpha", "beta", "gamma", 0 };
    long argChild = userSpawn("/bin/argtest", argtestArgv);
    if (argChild < 0) {
        print("proctest: FAIL spawn /bin/argtest\n");
        return 1;
    }

    long argStatus = userWait(argChild);
    if (argStatus != 0) {
        print("proctest: FAIL argtest reported bad argv\n");
        return 1;
    }
    print("proctest: spawn with argv ok\n");

    userExec("/bin/exectarget", 0);

    /* Only reached if userExec failed outright (bad path, load failure) --
       a successful exec never returns here at all. */
    print("proctest: FAIL exec did not take over\n");
    return 1;
}
