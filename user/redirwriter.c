#include "ulib.h"

/*
 * Live ring-3 proof for shell.c's new `cmd > file` / `cmd >> file` file
 * redirection (jobctlApplyRedirects/jobctlSpawnForegroundRedirected,
 * jobctl.h): from this program's own point of view it just writes its
 * stdout exactly like pipesrc.c already does for the pipe case -- it has
 * no idea whether fd 1 is the real console, a pipe, or (here) a real file
 * on tmpfs, the same "redirection is invisible to the child" property a
 * real shell's dup2()-before-exec gives every Unix program. fd 2 is
 * deliberately left alone (only fd 0/fd 1 are ever redirected, matching
 * jobctlSpawnPiped's own pipeline convention), so the diagnostic line
 * below reaches the real console even while fd 1's own payload goes to
 * the file instead -- writing it through fd 1 would corrupt the exact
 * bytes redirreader.c later compares.
 *
 * kmain.c's redirDriverThread runs this program TWICE against the same
 * path -- once through `>` (truncate/create) and once through `>>`
 * (append) -- so the resulting file holds two back-to-back copies of this
 * same payload with no separator, proving `>>` genuinely continues from
 * the end of what `>` already wrote rather than overwriting it.
 */
#define redirectPayload "redirect-payload-ok"

static void printErr(const char* text)
{
    userWrite(2, text, stringLength(text));
}

int main(void)
{
    printErr("redirwriter: writing payload to stdout (redirected)\n");

    unsigned long length = stringLength(redirectPayload);
    long written = userWrite(1, redirectPayload, length);
    if (written != (long)length) {
        printErr("redirwriter: FAIL short/failed write\n");
        return 1;
    }

    printErr("redirwriter: wrote payload\n");
    return 0;
}
