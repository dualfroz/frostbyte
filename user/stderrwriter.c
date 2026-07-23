#include "ulib.h"

/*
 * Live ring-3 proof for shell.c's new `cmd 2> file` / `cmd 2>> file` stderr
 * redirection (jobctlApplyRedirects/jobctlSpawnForegroundRedirected,
 * jobctl.h): the exact mirror image of redirwriter.c's own `>`/`>>` proof,
 * with fd 1 and fd 2's roles swapped -- this program writes its payload to
 * fd 2 (the fd the shell redirects here) and its own diagnostics to fd 1
 * (which stays on the real console in this demo, since only fd 2 is ever
 * redirected). Writing the diagnostics through fd 2 would corrupt the exact
 * bytes stderrreader.c later compares, the same hazard redirwriter.c avoids
 * by keeping its own diagnostics off fd 1.
 *
 * kmain.c's stderrDriverThread runs this program TWICE against the same
 * path -- once through `2>` (truncate/create) and once through `2>>`
 * (append) -- so the resulting file holds two back-to-back copies of this
 * same payload with no separator, proving `2>>` genuinely continues from
 * the end of what `2>` already wrote rather than overwriting it.
 */
#define stderrPayload "stderr-payload-ok"

static void printOut(const char* text)
{
    userWrite(1, text, stringLength(text));
}

int main(void)
{
    printOut("stderrwriter: writing payload to stderr (redirected)\n");

    unsigned long length = stringLength(stderrPayload);
    long written = userWrite(2, stderrPayload, length);
    if (written != (long)length) {
        printOut("stderrwriter: FAIL short/failed write\n");
        return 1;
    }

    printOut("stderrwriter: wrote payload\n");
    return 0;
}
