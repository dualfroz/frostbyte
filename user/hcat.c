#include "ulib.h"

/*
 * Left half of shell.c's new `cmd1 <<< TEXT | cmd2` here-string-into-
 * pipeline live demo -- jobctl.c's new jobctlSpawnPipedHereString wires
 * THIS process's fd 0 to a real pipe the shell itself writes the typed
 * `<<<` text into, AND its fd 1 to the pipeline's shared pipe (right
 * half's fd 0), both BEFORE this process ever runs a single instruction.
 * From this program's own point of view there is nothing special about
 * either fd: it is simply "cat" -- read stdin to real EOF, then write
 * exactly those bytes to stdout -- with no idea either end is a pipe
 * rather than a real file or console. Named "hcat" (not "cat") purely so
 * the driver thread's typed line (binary name + `<<<` + text + `|` +
 * binary name) stays clearly under keyboard.c's 32-byte
 * consoleReaderBufferSize per line -- see kmain.c's FALA 31 lesson,
 * documented right where stderrDriverThread's own path length once
 * crossed that exact boundary.
 *
 * Diagnostics go to fd 2 (never fd 1) for the same reason pipesrc.c's own
 * printErr does: fd 1 here is the real pipeline payload hsink.c compares
 * byte-for-byte, and print()/printDec() (ulib.h) are both hardcoded to
 * fd 1.
 */

static void printErr(const char* text)
{
    userWrite(2, text, stringLength(text));
}

int main(void)
{
    printErr("hcat: reading here-string stdin\n");

    char buffer[64];
    unsigned long total = 0;
    for (;;) {
        long got = userRead(0, buffer + total, sizeof(buffer) - 1 - total);
        if (got < 0) {
            printErr("hcat: FAIL read\n");
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
        printErr("hcat: FAIL short/failed write\n");
        return 1;
    }

    printErr("hcat: forwarded payload\n");
    return 0;
}
