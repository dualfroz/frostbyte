#include "ulib.h"

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
