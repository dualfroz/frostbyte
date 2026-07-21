#include "ulib.h"

/*
 * Never spawned directly by kmain.c the way every other boot demo in this
 * kernel is -- it just sits in /bin like any other binary until kmain.c's
 * jobctlDriverThread types "jobctlfg" at the REAL interactive shell
 * (shell.c) through injected keystrokes (same "no automated way to drive a
 * real PS/2 device under headless QEMU" workaround every other keyboard
 * demo in this file already uses). shell.c's runProgram falls back to
 * jobctlSpawnForeground (jobctl.c) for any word that isn't one of its
 * built-ins, which is the first time this kernel has ever loaded a binary
 * on demand from user input rather than eagerly at boot -- a real shell's
 * $PATH lookup, not another pre-arranged spawnUser call.
 *
 * jobctlSpawnForeground makes this process its own new group leader and
 * calls keyboardSetForegroundPgid on its behalf BEFORE the shell blocks
 * waiting for it (jobctlWaitForeground) -- so this file's proof is exactly
 * pgroupfg.c's own (an uncaught, real, injected Ctrl-C actually reaches
 * "the foreground" and terminates it by default disposition), but now
 * "the foreground" was granted by the SHELL's own runtime job-control
 * decision instead of a single hardcoded kmain.c call. No handler
 * installed for sigInt at all: the real-world default disposition
 * (signal.c's sigDefaultIsIgnore does not list sigInt) is Terminate,
 * exactly like a real uncaught Ctrl-C killing a foreground program.
 */

int main(void)
{
    print("jobctlfg: start, pid=");
    printDec(userGetpid());
    print("\n");

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("jobctlfg: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("jobctlfg: FAIL attach victim pipe\n");
        return 1;
    }

    print("jobctlfg: ready, blocking on go pipe\n");
    char buffer[4];
    userRead((int)victimFd, buffer, sizeof(buffer));

    /* Only reached if userRead somehow returned on its own -- nothing ever
       writes to the victim pipe, so the shell-granted foreground Ctrl-C
       never arriving (or arriving but not actually terminating this
       process) is the only way to get here. */
    print("jobctlfg: FAIL read returned without being interrupted\n");
    return 1;
}
