#include "ulib.h"

/*
 * Exercises argv/argc plumbing added this wave (elfBuildInitialStack in
 * elf.c, copyUserArgv in syscall.c, crt.S's _start now reading argc/argv
 * off the initial stack instead of calling main() with nothing). Only ever
 * spawned by proctest.c, with a specific, known argv -- if the kernel-side
 * stack layout is wrong in any way (misaligned rsp, wrong argc, a string
 * copied to the wrong address, an argv[i] pointer landing on garbage),
 * these comparisons catch it directly instead of a lucky match papering
 * over a bug. userspace has no libc string.h, so the comparison is spelled
 * out by hand exactly the way spawntarget.c/proctest.c already avoid
 * pulling in anything beyond ulib.h.
 */

static int streq(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(int argc, char** argv)
{
    print("argtest: argc=");
    printDec(argc);
    print("\n");

    for (int i = 0; i < argc; i++) {
        print("argtest: argv[");
        printDec(i);
        print("]=");
        print(argv[i]);
        print("\n");
    }

    int ok = argc == 4 &&
        streq(argv[0], "/bin/argtest") &&
        streq(argv[1], "alpha") &&
        streq(argv[2], "beta") &&
        streq(argv[3], "gamma");

    print(ok ? "argtest: argv-ok\n" : "argtest: argv-FAIL\n");
    return ok ? 0 : 1;
}
