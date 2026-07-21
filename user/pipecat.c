#include "ulib.h"

/*
 * Middle stage of shell.c's new N-stage pipeline live demo
 * (`pipesrc | pipecat | pipesink`) -- the first process in this kernel with
 * BOTH ends of two DIFFERENT real inter-process pipes baked into its fds at
 * once (fd 0 from jobctlSpawnPiped's stdinHandle, fd 1 from its
 * stdoutHandle). user/hcat.c already proved a piped stdout alongside a
 * piped stdin, but that stdin was always a shell-fed here-string
 * (jobctlSpawnPipedHereString), never a second real process-to-process
 * pipe the way this one's is -- this program is the actual proof that
 * shell.c's runPipelineStages can wire a genuine middle pipeline stage
 * where NEITHER end is the console. From this program's own point of view
 * there is nothing special about either fd: it is simply "cat" -- read
 * stdin to real EOF, then write exactly those bytes to stdout -- with no
 * idea either end is a pipe rather than a real file or console, the same
 * shape as hcat.c's/pipesink.c's own read loops.
 *
 * Diagnostics go to fd 2 (never fd 1), same reason as every other filter in
 * this pipeline family: fd 1 here is the real payload pipesink.c compares
 * byte-for-byte, and print()/printDec() (ulib.h) are both hardcoded to fd 1.
 */

static void printErr(const char* text)
{
    userWrite(2, text, stringLength(text));
}

int main(void)
{
    printErr("pipecat: reading piped stdin\n");

    char buffer[64];
    unsigned long total = 0;
    for (;;) {
        long got = userRead(0, buffer + total, sizeof(buffer) - 1 - total);
        if (got < 0) {
            printErr("pipecat: FAIL read\n");
            return 1;
        }
        if (got == 0)
            break;
        total += (unsigned long)got;
        if (total >= sizeof(buffer) - 1)
            break;
    }

    long written = userWrite(1, buffer, total);
    if (written != (long)total) {
        printErr("pipecat: FAIL short/failed write\n");
        return 1;
    }

    printErr("pipecat: forwarded payload\n");
    return 0;
}
