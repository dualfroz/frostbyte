#include "ulib.h"

#define pipelinePayload "pipeline-payload-ok"

static void printErr(const char* text)
{
    userWrite(2, text, stringLength(text));
}

int main(void)
{
    printErr("pipesrc: writing payload to stdout (pipe)\n");

    unsigned long length = stringLength(pipelinePayload);
    long written = userWrite(1, pipelinePayload, length);
    if (written != (long)length) {
        printErr("pipesrc: FAIL short/failed write\n");
        return 1;
    }

    printErr("pipesrc: wrote payload\n");
    return 0;
}
