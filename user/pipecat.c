#include "ulib.h"

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
