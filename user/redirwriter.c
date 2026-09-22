#include "ulib.h"

#define redirectPayload "redirect-payload-ok"

static void printErr(const char* text)
{
    userWrite(2, text, stringLength(text));
}

int main(void)
{
    printErr("redirwriter: writing payload to stdout (redirected)\n");

    unsigned long length = stringLength(redirectPayload);
    long written = userWrite(1, redirectPayload, length);
    if (written != (long)length) {
        printErr("redirwriter: FAIL short/failed write\n");
        return 1;
    }

    printErr("redirwriter: wrote payload\n");
    return 0;
}
