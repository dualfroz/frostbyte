#include "ulib.h"

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
