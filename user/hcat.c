#include "ulib.h"

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
