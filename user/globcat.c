#include "ulib.h"

static void printErr(const char* text)
{
    userWrite(2, text, stringLength(text));
}

int main(int argc, char** argv)
{
    char countText[24];
    decToString(argc - 1, countText);
    printParts((const char* const[]){ "globcat: got ", countText, " files\n", 0 });

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

        printParts((const char* const[]){ "globcat: ", argv[i], "=", buffer, 0 });
    }

    return 0;
}
