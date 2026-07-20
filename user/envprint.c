#include "ulib.h"

/*
 * Live ring-3 proof for the real environment-variable subsystem (sched.h's
 * Thread.env, syscall.c's sysGetEnv/sysSetEnv/sysUnsetEnv, shell.c's
 * `export`/`env`/`unset` builtins and `$NAME` expansion) -- loaded on demand by
 * the real interactive shell (kmain.c's envDriverThread types
 * "export GREETING=frostbyte" then "envprint" at it, the same injected-
 * keystroke mechanism every shell-driven demo above already uses), never
 * boot-spawned, the same "no kmain.c spawnUser call, only /bin/NAME
 * resolved by the shell's own fallback" shape jobctlfg.c/hcat.c/pipecat.c
 * already established.
 *
 * Proves inheritance first (this process never called userSetEnv itself
 * before the first check -- whatever it sees came only from
 * threadCreateUser's own initialEnv copying the shell's table at spawn
 * time), then proves this process's OWN later userSetEnv is visible to
 * itself immediately, then proves userUnsetEnv actually removes the entry
 * (a following userGetEnv on the same name fails) and that unsetting a
 * name that was never set is still a no-op success, not an error. The
 * other half of the isolation guarantee --  that
 * this process's own mutation can never reach back into the PARENT
 * shell's copy -- is proven by envDriverThread itself: it types a plain
 * "env" at the shell AFTER this process has already exited and checks
 * that the shell still reports the ORIGINAL "frostbyte" value, not
 * "child-mutated" (mirrors cwdtest.c's own post-userWait
 * parent-cwd-unaffected check, just driven through the shell instead of a
 * direct userWait).
 */

int main(void)
{
    print("envprint: start, pid=");
    printDec(userGetpid());
    print("\n");

    char value[32];
    long len = userGetEnv("GREETING", value, sizeof(value));
    if (len < 0) {
        print("envprint: FAIL missing inherited GREETING\n");
        return 1;
    }
    print("envprint: inherited-ok GREETING=");
    print(value);
    print("\n");

    if (userGetEnv("NO_SUCH_VAR", value, sizeof(value)) >= 0) {
        print("envprint: FAIL unset variable returned a value\n");
        return 1;
    }

    if (userSetEnv("GREETING", "child-mutated") != 0) {
        print("envprint: FAIL setenv\n");
        return 1;
    }
    len = userGetEnv("GREETING", value, sizeof(value));
    if (len < 0) {
        print("envprint: FAIL read back own setenv\n");
        return 1;
    }
    print("envprint: self-set-ok GREETING=");
    print(value);
    print("\n");

    if (userUnsetEnv("GREETING") != 0) {
        print("envprint: FAIL unset\n");
        return 1;
    }
    if (userGetEnv("GREETING", value, sizeof(value)) >= 0) {
        print("envprint: FAIL unset did not remove variable\n");
        return 1;
    }
    print("envprint: unset-ok\n");

    /* Real unsetenv(3) is a no-op success on a name that was never set --
       this must NOT be confused with a "not found" error. */
    if (userUnsetEnv("NO_SUCH_VAR_EVER") != 0) {
        print("envprint: FAIL unset-missing should be a no-op success\n");
        return 1;
    }
    print("envprint: unset-missing-ok\n");

    return 0;
}
