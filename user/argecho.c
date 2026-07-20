#include "ulib.h"

/*
 * Real ring-3 proof that shell.c's `$NAME`/`$?` expansion (shellExpandArgv,
 * shellExpandToken) genuinely reaches a SPAWNED program's own argv, not
 * just a shell builtin's -- every earlier consumer of expansion (export,
 * env, echo) is a builtin, and shell.c's doEcho never calls shellExpandArgv
 * at all (only runProgram/runPipelineStages do, for a real spawned
 * process), so nothing in this kernel's live-demo chain had ever actually
 * typed a `$` at the real shell before this program existed. Prints each
 * argv[1..argc) on its own line, prefixed like globcat.c's own
 * "globcat: " diagnostic style -- a generic argv printer, not tied to any
 * one variable, so a future demo can reuse it for any other `$NAME`/`$?`
 * case without needing yet another single-purpose binary.
 */
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        print("argecho: ");
        print(argv[i]);
        print("\n");
    }
    return 0;
}
