#include "ulib.h"

/*
 * Live proof that shell.c's new wildcard glob expansion (shellExpandGlobs)
 * really hands a spawned process REAL, distinct filenames resolved from a
 * pattern typed at the shell (`globcat /tmp/gd/ *.txt`) -- not a literal,
 * unexpanded "*.txt" string, and not every file in the directory (the
 * sibling /tmp/gd/c.log the driver thread also creates must NOT appear).
 * Prints how many files it actually received, then each one's own path and
 * content, all on fd 1 (unlike pipecat.c/hcat.c, this program's own argv
 * and stdout ARE the thing being demonstrated, not a pipe payload it must
 * keep pristine).
 */

static void printErr(const char* text)
{
    userWrite(2, text, stringLength(text));
}

int main(int argc, char** argv)
{
    print("globcat: got ");
    printDec(argc - 1);
    print(" files\n");

    for (int i = 1; i < argc; i++) {
        long fd = userOpen(argv[i], oReadOnly);
        if (fd < 0) {
            printErr("globcat: FAIL cannot open ");
            printErr(argv[i]);
            printErr("\n");
            return 1;
        }

        char buffer[64];
        long got = userRead((int)fd, buffer, sizeof(buffer) - 1);
        userClose((int)fd);
        if (got < 0) {
            printErr("globcat: FAIL read\n");
            return 1;
        }
        buffer[got] = 0;

        print("globcat: ");
        print(argv[i]);
        print("=");
        print(buffer);
    }

    return 0;
}
